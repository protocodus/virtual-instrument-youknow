#!/usr/bin/env python3
"""Audit numerical chorus-noise reconstruction, not installed-unit calibration.

Build AuditChorusNoise.cpp against both e405d7a and the candidate. Keep each
--measure directory, and export --support from the candidate. This script
checks the 768 kHz reference against the analytical held-white PSD, then
compares every lower rate against that reference using the identical random
sequence. No gain, clock, color or other parameter is fitted. The non-integer
37.001 kHz and 100 kHz clocks are held-out checks; 20/80 kHz span the design
cases. All bands through 16 kHz are reported, including the remaining coarse
grid attenuation from the finite Lagrange reconstruction kernel.

The optional pinned hardware input reports ONLY the original A11 pre-roll
before its first chord at 0.25s. Neither a gain reference, a clock measurement,
nor sufficient independent silence is available to calibrate the noise law.
The owner video shows the KR106 UI and has AAC audio; its routing/processing
cannot identify original-unit noise, so it is not a hardware fitting source.

Primary sample-and-hold derivation: Christian Enz, Noise Sampling, section 3,
https://www.ekvmodel.com/notebooks/Noise%20Sampling/Noise_sampling.html
For the existing iid edge amplitudes of variance v, time-averaged continuous
two-sided PSD is v/fcp*sinc(f/fcp)^2. No kT/C amplitude is inferred here.

Usage: python3 Tools/AnalyzeChorusNoise.py --baseline A_DIR --candidate B_DIR
    --support support.json --hardware bank_A1x.wav --output report.json
Requires NumPy/SciPy. The C++ quadrature test is a separate deterministic
oracle and does not depend on spectral-estimator statistical tolerances.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import platform
import numpy as np
import scipy
from scipy.io import wavfile
from scipy.signal import welch

BANDS = [(200,1000),(1000,4000),(4000,8000),(8000,12000),(12000,16000)]
PCM_HASH = "1ed7f92f6fe213ed9d0fdea13e133c8ee2e4e0b3f202b535eb7a9d7c5a703772"

def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def spectrum(samples,rate):
    return welch(samples,rate,nperseg=rate//10,axis=0)

def powers(frequencies,density):
    return np.asarray([density[(frequencies>=low)&(frequencies<high)].sum(axis=0)
                       *(frequencies[1]-frequencies[0]) for low,high in BANDS])

def response(support,frequencies):
    z=np.exp(-2j*np.pi*frequencies/support['sample_rate'])
    state=np.asarray(support['state_by_column']).T
    drive=np.asarray(support['drive_by_sample'])
    rhs=np.einsum('fk,ki->fi',z[:,None]**np.arange(4),drive)
    solved=np.linalg.solve(np.eye(6)[None,:,:]-z[:,None,None]*state,rhs[:,:,None])[:,:,0]
    return solved[:,4]-solved[:,5]

def read_directory(directory):
    result={}
    for row in csv.DictReader((directory/'index.csv').open()):
        rate,clock=int(row['sample_rate']),int(row['clock_hz'])
        samples=np.fromfile(directory/row['file'],dtype='<f4')
        assert len(samples)==int(float(row['seconds'])*rate) and np.isfinite(samples).all()
        f,p=spectrum(samples[int(float(row['settle_seconds'])*rate):],rate)
        result[rate,clock]={'power':powers(f,p),'amplitude':float(row['source_amplitude']),
                            'file_sha256':sha(directory/row['file'])}
    assert set(result)=={(r,c) for r in [44100,48000,96000,192000,768000]
                        for c in [20000,37001,80000,100000]}
    return result

def hardware_observations(path):
    rate,samples=wavfile.read(path)
    assert rate==96000 and samples.ndim==2 and samples.shape[1]==2
    if samples.dtype.kind=='i': samples=samples.astype(float)/(1 << (8*samples.dtype.itemsize-1))
    else: samples=samples.astype(float)
    assert hashlib.sha256(np.asarray(samples,dtype='<f4').tobytes()).hexdigest()==PCM_HASH
    result={'source':'https://github.com/kayrockscreenprinting/ultramaster_kr106/issues/16#issuecomment-4184997000',
            'file_sha256':sha(path),'pcm_sha256':PCM_HASH,'windows':[],
            'calibration_qualified':False,
            'limitations':['first chord at 0.25s leaves only a short pre-roll',
                           'chorus/mute switch history and recording gain are unknown',
                           'no direct clock trace or independently identified chip noise PSD',
                           'later early gaps contain musical release tails; no noise-law fit applied']}
    for start,end in [(0,.1),(.1,.2)]:
        x=samples[int(start*rate):int(end*rate)]
        f,p=spectrum(x,rate)
        result['windows'].append({'seconds':[start,end],
            'rms_dbfs':(20*np.log10(np.sqrt(np.mean(x*x,axis=0)))).tolist(),
            'band_dbfs':(10*np.log10(powers(f,p))).tolist(),
            'stereo_correlation':float(np.corrcoef(x.T)[0,1])})
    return result

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','candidate','support','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--hardware',type=Path)
    args=parser.parse_args()
    if args.output.exists(): raise RuntimeError('output already exists; preserve previous evidence')
    baseline,candidate=read_directory(args.baseline),read_directory(args.candidate)
    support=json.loads(args.support.read_text())
    rows=[]; recovery=[]
    for clock in [20000,37001,80000,100000]:
        reference=candidate[768000,clock]
        f=np.arange(0,384001,10,dtype=float)
        # Solve only frequencies that enter the declared measurement bands.
        mask=f<16000
        density=np.zeros_like(f)
        density[mask]=(2*reference['amplitude']**2/(3*clock)*np.sinc(f[mask]/clock)**2
                       *abs(response(support,f[mask]))**2)
        theory=powers(f,density)
        recovery.append({'clock_hz':clock,'band_error_db':(10*np.log10(reference['power']/theory)).tolist()})
        for rate in [44100,48000,96000,192000]:
            assert baseline[rate,clock]['amplitude']==candidate[rate,clock]['amplitude']
            rows.append({'sample_rate':rate,'clock_hz':clock,'held_out':clock in [37001,100000],
                'baseline_error_db':(10*np.log10(baseline[rate,clock]['power']/reference['power'])).tolist(),
                'candidate_error_db':(10*np.log10(candidate[rate,clock]['power']/reference['power'])).tolist(),
                'baseline_sha256':baseline[rate,clock]['file_sha256'],
                'candidate_sha256':candidate[rate,clock]['file_sha256']})
    known_max=max(abs(error) for row in recovery for error in row['band_error_db'])
    summaries={}
    for name,selected in [('all',rows),('held_out',[r for r in rows if r['held_out']])]:
        summaries[name]={}
        for profile in ['baseline','candidate']:
            errors=np.asarray([r[profile+'_error_db'] for r in selected])
            summaries[name][profile]={'maximum_absolute_band_error_db':float(abs(errors).max()),
                'mean_absolute_band_error_db':float(abs(errors).mean()),
                'maximum_absolute_200_8000_hz_band_error_db':float(abs(errors[:,:3]).max())}
    result={'scope':'numerical reconstruction of the existing held BBD source; no installed-unit amplitude/color fit',
            'versions':{'python':platform.python_version(),'numpy':np.__version__,'scipy':scipy.__version__},
            'analyzer_sha256':sha(__file__),'support_sha256':sha(args.support),'bands_hz':BANDS,
            'known_model_recovery':recovery,'known_model_maximum_absolute_error_db':known_max,
            'known_model_screen_passes':known_max<0.75,'comparisons':rows,'summary':summaries,
            'remaining_limit':'finite reconstruction kernel attenuates 12–16kHz by about1dB at 44.1kHz; higher-rate paths reduce this numerical error',
            'reference_file_sha256':{str(c):candidate[768000,c]['file_sha256'] for c in [20000,37001,80000,100000]}}
    if args.hardware: result['hardware']=hardware_observations(args.hardware)
    args.output.write_text(json.dumps(result,indent=2,allow_nan=False)+'\n')
    print(json.dumps({'known_model_maximum_absolute_error_db':known_max,'summary':summaries},indent=2))
    if not result['known_model_screen_passes']: raise RuntimeError('known-source PSD screen failed')

if __name__=='__main__': main()
