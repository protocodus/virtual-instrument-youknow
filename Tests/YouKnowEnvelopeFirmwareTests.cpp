#include "../Source/DSP/YouKnowEngine.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace youknow {
struct YouKnowTestAccess {
    using Envelope = YouKnowEngine::Envelope;
    static std::uint16_t level(const YouKnowEngine& engine, int card = 0) {
        return engine.voices_[static_cast<std::size_t>(card)].envelope.level;
    }
};
}
namespace {
using youknow::YouKnowEngine;
using Envelope = youknow::YouKnowTestAccess::Envelope;
int failures = 0;
void require(bool condition, const char* message) {
    if (!condition && failures++ < 12) std::cerr << message << '\n';
}
struct Oracle {
    std::uint16_t level = 0;
    bool atk = false, dcy = false, phase = false, gate = false, run = false;
    bool hold = false;

    // Independent B-2 083D..0850 partial products. The low*low term and
    // the fractional pieces of both cross terms are discarded separately.
    static std::uint16_t product(std::uint16_t x, std::uint16_t k) {
        const unsigned b=x/256, c=x%256, hi=k/256, lo=k%256;
        return static_cast<std::uint16_t>(b*hi+b*lo/256+c*hi/256);
    }
    void event(int kind) {
        switch (kind) {
        case 1: gate=atk=true; if(run) phase=false; break; // 0115..013B
        case 2: gate=false; if(!hold) phase=false; break; // 009F..00B5
        case 3: hold=true; break;                        // 00BA
        case 4: hold=false; break;                       // 00BF
        case 5: if(hold) run=run||gate; else run=gate; break; // 02F2..02FF
        }
    }
    void tick(std::uint16_t a, std::uint16_t d, std::uint16_t s, std::uint16_t r) {
        // Execute the decision flow by instruction address, independently of
        // the production predicate. ONAW skips on nonzero AND; OFFAW skips
        // on zero AND (NEC uPD7810/11 p23). The state is kept as three bits.
        // https://github.com/ErroneousBosh/j106roms/blob/26926a04ff1939106820313e71e34b4ca2f67070/ic29.txt#L825-L897
        unsigned pc=0x0509;
        for (int guard=0;guard<32;++guard) {
            switch(pc) {
            case 0x0509: pc=run?0x050e:0x050c; break;
            case 0x050c: pc=0x0538; break;
            case 0x050e: pc=phase?0x0513:0x0511; break;
            case 0x0511: pc=0x0563; break;
            case 0x0513: pc=atk?0x0517:0x0516; break;
            case 0x0516: pc=0x051e; break;
            case 0x0517: atk=false; pc=0x051e; break;
            case 0x051e:
                level=static_cast<std::uint16_t>(s+product(level>s?level-s:0,d));
                return;
            case 0x0538: pc=atk?0x053c:0x053b; break;
            case 0x053b: pc=0x054a; break;
            case 0x053c: pc=dcy?0x053f:0x0541; break;
            case 0x053f: pc=0x0517; break;
            case 0x0541: atk=false; pc=0x054a; break;
            case 0x054a: phase=dcy=false; level=product(level,r); return;
            case 0x0563: {
                dcy=false;
                const unsigned sum=level+a;
                if(sum&0xc000) { level=0x3fff; phase=dcy=true; }
                else level=static_cast<std::uint16_t>(sum);
                return;
            }
            default: std::abort();
            }
        }
        std::abort();
    }
};
void event(Envelope& env, int kind, bool& hold) {
    switch(kind) {
    case 1:env.noteOn();break;
    case 2:env.noteOff(hold);break;
    case 3:hold=true;break;
    case 4:hold=false;break;
    case 5:env.latchGate(hold);break;
    }
}
bool equal(const Envelope& e,const Oracle& o) {
    return e.level==o.level && e.attackPhase==o.atk && e.decayPhase==o.dcy
        && e.phase==o.phase && e.gate==o.gate && e.running==o.run;
}
void testDecisionTable() {
    for(unsigned bits=0;bits<32;++bits)
        for(auto level : {0u,1u,8191u,16362u,16383u})
            for(auto coefficients : {std::array<std::uint16_t,4>{0x4000,0x1000,0,0xfff4},
                    {127,0xfff4,0x2000,0x1000}, {21,0xff80,0x3f80,0xff00}}) {
                Envelope e;
                e.level=static_cast<std::uint16_t>(level);
                e.attackPhase=(bits&1)!=0;e.decayPhase=(bits&2)!=0;e.phase=(bits&4)!=0;
                e.gate=(bits&8)!=0;e.running=(bits&16)!=0;
                Oracle o{e.level,e.attackPhase,e.decayPhase,e.phase,e.gate,e.running};
                for(int pass=0;pass<4;++pass) {
                    e.tick(coefficients[0],coefficients[1],coefficients[2],coefficients[3]);
                    o.tick(coefficients[0],coefficients[1],coefficients[2],coefficients[3]);
                    require(equal(e,o),"envelope phase decision differs from B-2 branch oracle");
                }
            }
}
void reachable(Envelope e,Oracle o,bool hold,int depth) {
    if(depth==0)return;
    for(int kind=0;kind<6;++kind) {
        auto actual=e;auto expected=o;auto h=hold;
        if(kind==0) {
            actual.tick(0x4000,0x1000,0x2000,0xfff4);
            expected.tick(0x4000,0x1000,0x2000,0xfff4);
        } else {event(actual,kind,h);expected.event(kind);}
        require(equal(actual,expected),"reachable note/HOLD/pass sequence differs from B-2");
        reachable(actual,expected,h,depth-1);
    }
}
void testWitnesses() {
    for(const auto data : {std::array<std::uint16_t,5>{0x1000,0xfff4,0,1023,1021},
            {0xfff4,0x1000,0,16379,1023},{0x1000,0xfff4,0x2000,8703,8700}}) {
        Envelope e;e.noteOn();e.latchGate(false);e.tick(0x4000,data[0],data[2],data[1]);
        e.noteOff();e.latchGate(false);e.tick(0x4000,data[0],data[2],data[1]);
        require(e.level==data[3],"attack-boundary note-off skipped retained decay");
        e.tick(0x4000,data[0],data[2],data[1]);
        require(e.level==data[4],"retained decay failed to enter release on following pass");
    }
    // A reattack during decay sets FF07, but does not clear FF08 until an
    // attack calculation actually executes. Key-up can arrive first.
    Envelope e;e.noteOn();e.latchGate(false);e.tick(0x4000,0x1000,8192,0xfff4);
    e.tick(0x4000,0x1000,8192,0xfff4);
    e.noteOn();e.noteOff();e.latchGate(false);e.tick(0x4000,0x1000,8192,0xfff4);
    require(e.level==8223,"unconsumed reattack discarded the previous decay latch");
}
void render(YouKnowEngine& engine,int frames,int block) {
    std::vector<float> left(static_cast<std::size_t>(block)),right(left.size());
    while(frames>0) {
        int n=std::min(frames,block);
        engine.process(left.data(),right.data(),n);
        frames-=n;
    }
}
void testEngineTiming() {
    for(double rate : {44100.0,48000.0,96000.0})
        for(int quality : {1,4}) {
            auto a=std::make_unique<YouKnowEngine>();auto b=std::make_unique<YouKnowEngine>();
            youknow::EngineParameters p;p.attack=0;p.decay=0;p.release=1;p.sustain=0;
            p.calibration=p.velocityDepth=0;
            for(auto* engine : {a.get(),b.get()}) {
                engine->setParameters(p);engine->prepare(rate,128,quality);
                engine->noteOn(60,1);
            }
            // Stop at the first completed physical envelope store. Release
            // before another store, independently of chart phase rounding.
            int frames=0;
            while(youknow::YouKnowTestAccess::level(*a)==0 && frames++<2000) render(*a,1,1);
            render(*b,frames,37);
            require(youknow::YouKnowTestAccess::level(*a)==0x3fff,"engine fixture missed first attack");
            for(auto* engine:{a.get(),b.get()})engine->noteOff(60);
            int next=0;
            while(youknow::YouKnowTestAccess::level(*a)==0x3fff && next++<2000)render(*a,1,1);
            render(*b,next,113);
            require(youknow::YouKnowTestAccess::level(*a)==1023,"renderer did not consume pending decay on key-up");
            require(youknow::YouKnowTestAccess::level(*a)==youknow::YouKnowTestAccess::level(*b),
                    "envelope boundary depends on host block partition");
        }
}
}
int main() {
    testDecisionTable();reachable({}, {},false,7);testWitnesses();testEngineTiming();
    if(failures) {std::cerr<<failures<<" envelope firmware failures\n";return 1;}
    std::cout<<"Envelope firmware: decision branches, reachable events, witnesses and renderer PASS\n";
}
