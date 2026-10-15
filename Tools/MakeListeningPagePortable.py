#!/usr/bin/env python3
"""Embed exact local WAVs and evidence in a listening page for sandbox previews.

Only HTML is rewritten. Audio, keys and manifests remain untouched. A preview
may have an opaque origin and no usable base URL, so relative media paths fail
even when every file is present. Native data-URI audio also works without JS.
"""

from __future__ import annotations

import argparse
import base64
import html
from html.parser import HTMLParser
import json
from pathlib import Path


MARKER = '<!-- youknow-portable-player-v1 -->'
EXTERNAL_MARKER = '<!-- youknow-external-player-v1 -->'


class PortablePage(HTMLParser):
    def __init__(self, page: Path, *, embed_audio: bool = True):
        super().__init__(convert_charrefs=False)
        self.folder = page.parent.resolve()
        self.embed_audio = embed_audio
        self.parts: list[str] = []
        self.documents: dict[str, dict[str, str]] = {}
        self.audio_count = 0

    def asset(self, relative: str) -> Path:
        path = (self.folder / relative).resolve()
        if not path.is_relative_to(self.folder) or not path.is_file():
            raise ValueError(f'Missing or non-local player asset: {relative}')
        return path

    def handle_starttag(self, tag, attrs):
        attributes = dict(attrs)
        changed = False
        if tag == 'audio' and 'src' in attributes:
            source = attributes['src']
            if not source.startswith('data:audio/'):
                data = self.asset(source).read_bytes()
                if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
                    raise ValueError(f'Expected a WAV file: {source}')
                if self.embed_audio:
                    attributes['src'] = 'data:audio/wav;base64,' + base64.b64encode(data).decode('ascii')
                attributes['data-audio-file'] = source
                attributes.setdefault('data-group', Path(source).parent.as_posix())
            # Load the selected clip on demand instead of decoding every clip.
            attributes['preload'] = 'none'
            self.audio_count += 1
            changed = True
        if tag == 'a' and 'href' in attributes:
            source = attributes['href']
            if not source.startswith(('#', 'data:', 'https:', 'http:')) and Path(source).suffix in ('.md', '.json'):
                path = self.asset(source)
                identity = f'document-{len(self.documents)}'
                self.documents[identity] = {'name': path.name, 'text': path.read_text()}
                attributes['href'] = '#player-document'
                attributes['data-document'] = identity
                changed = True
        if changed:
            self.parts.append('<' + tag + ''.join(
                ' ' + name + ('' if value is None else '="' + html.escape(value, quote=True) + '"')
                for name, value in attributes.items()) + '>')
        else:
            self.parts.append(self.get_starttag_text())

    def handle_startendtag(self, tag, attrs):
        self.parts.append(self.get_starttag_text())

    def handle_endtag(self, tag):
        self.parts.append(f'</{tag}>')

    def handle_data(self, data):
        self.parts.append(data)

    def handle_entityref(self, name):
        self.parts.append(f'&{name};')

    def handle_charref(self, name):
        self.parts.append(f'&#{name};')

    def handle_comment(self, data):
        self.parts.append(f'<!--{data}-->')

    def handle_decl(self, decl):
        self.parts.append(f'<!{decl}>')


def add_player(page: Path, *, embed_audio: bool) -> int:
    source = page.read_text()
    if MARKER in source or EXTERNAL_MARKER in source:
        return 0
    parser = PortablePage(page, embed_audio=embed_audio)
    parser.feed(source)
    parser.close()
    if not parser.audio_count:
        raise ValueError(f'No audio players in {page}')
    documents = json.dumps(parser.documents, ensure_ascii=True).replace('<', '\\u003c')
    playback_error = (
        'This preview could not play the embedded audio. Download this HTML page and open it in your browser; the audio is included in the file.'
        if embed_audio else
        'This preview could not play the audio files. Open the portable pair link, or serve the complete listening package locally.'
    )
    support = (MARKER if embed_audio else EXTERNAL_MARKER) + '''
<style>
#playback-error{position:fixed;bottom:1rem;left:1rem;right:1rem;z-index:10;padding:1rem;background:#50212a;color:#fff;border-radius:.5rem}
#player-document{width:min(800px,85vw);max-height:80vh;padding:1.5rem;border:1px solid #526577;border-radius:12px;background:#17212b;color:#e7edf3}
#player-document::backdrop{background:#0009}#player-document pre{white-space:pre-wrap;overflow-wrap:anywhere;font:14px/1.5 ui-monospace,monospace}
#player-document button{float:right;font:inherit;padding:.5rem 1rem;cursor:pointer}
</style>
<p id="playback-error" role="alert" hidden></p>
<dialog id="player-document" aria-labelledby="player-document-title"><button type="button" id="close-document">Close</button><h2 id="player-document-title"></h2><pre id="player-document-text"></pre></dialog>
<script>
(() => {
  const documents = DOCUMENTS;
  const dialog = document.getElementById('player-document');
  document.querySelectorAll('[data-document]').forEach(link => {
    link.addEventListener('click', event => {
      event.preventDefault();
      const item = documents[link.dataset.document];
      document.getElementById('player-document-title').textContent = item.name;
      document.getElementById('player-document-text').textContent = item.text;
      dialog.showModal();
    });
  });
  document.getElementById('close-document').addEventListener('click', () => dialog.close());
  const error = document.getElementById('playback-error');
  let active = null;
  document.querySelectorAll('audio').forEach(audio => {
    audio.addEventListener('play', () => {
      const previous = active;
      active = audio;
      if (previous && previous !== audio) {
        const resumeAt = previous.currentTime;
        const switchPair = previous.dataset.group === audio.dataset.group && !previous.paused;
        previous.pause();
        if (switchPair) {
          const seek = () => {
            if (active === audio) audio.currentTime = Math.min(resumeAt, audio.duration);
          };
          if (audio.readyState >= 1) seek();
          else audio.addEventListener('loadedmetadata', seek, {once: true});
        }
      }
    });
    audio.addEventListener('error', () => {
      error.hidden = false;
      error.textContent = PLAYBACK_ERROR;
    });
    audio.addEventListener('playing', () => { error.hidden = true; });
  });
})();
</script>
'''.replace('DOCUMENTS', documents).replace('PLAYBACK_ERROR', json.dumps(playback_error))
    result = ''.join(parser.parts)
    end = result.rfind('</body>')
    if end < 0:
        end = result.rfind('</html>')
    if end < 0:
        end = len(result)
    page.write_text(result[:end] + support + result[end:])
    return parser.audio_count


def make_portable(page: Path) -> int:
    """Embed media and evidence, retaining the original standalone-page API."""
    return add_player(page, embed_audio=True)


def make_external(page: Path) -> int:
    """Add the same playback controls while leaving WAVs as relative URLs."""
    return add_player(page, embed_audio=False)


if __name__ == '__main__':
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('pages', type=Path, nargs='+')
    for page in args.parse_args().pages:
        count = make_portable(page.resolve())
        print(f'{page}: embedded {count} players')
