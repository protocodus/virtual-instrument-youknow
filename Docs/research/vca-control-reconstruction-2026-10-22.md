# VCA current reconstruction

The selected C58/Tr20 circuit already advanced physical capacitor charge at
fractional converter timestamps. The voice filter island then interpolated
only its endpoint gains. This pass reconstructs the curved current between
those same endpoints, splitting at the actual converter write. It changes no
stored control, VCA LEVEL mapping, service gain, resistor, capacitor or
transistor parameter.

Roland's [module-board drawing, p. 13](https://www.kiwitechnics.com/downloads/Kiwi-106/Roland%20Juno-106%20Service%20Manual.pdf#page=13)
supplies R106 10 kOhm, R105 22 kOhm and C58 0.1 uF. The implementation
reuses the existing named Tr20 specimen and thermal state; it adds no new
component estimate. With normalized capacitor charge `q`, equivalent settled
control `u`, emitter current `Ie` and fixed service current `Iservice`,

```
dq/dt = (target-u)/(R106*C58)
dG/dt = G*span*(target-u)/((R106*C58)*(Vt+R105*Ie))
G = Ie/Iservice
```

The physical endpoint derivatives feed bounded monotone Hermite segments.
One segment ends and another starts at a converter event. Their endpoints,
including declared Off and settled gain, are exact. The existing FIR delay
carries the reconstruction to the same subinterval as audio. Warm numerical
history is held at one gain, avoiding replay of an old physical interval.
The comparison-only upstream envelope-hold configuration retains its former
interpolation. The raw engine leaves the new product flag disabled.

`YouKnowVcaControlReconstructionTests` uses a separate voltage-domain junction
solve and fine-step capacitor KCL integration. It checks fine-grid convergence,
onset/release over 25, 41.37 and 63 C, four control transitions, and converter
positions 0, .17, .53 and 1. Initial focused results:

| Internal rate | Aggregate error reduction | Maximum absolute gain error |
| --- | ---: | ---: |
| 8 kHz stress | 21.26 dB | 0.004949 |
| 44.1 kHz | 37.28 dB | 0.0001670 |
| 48 kHz | 38.52 dB | 0.0001331 |
| 96 kHz | 50.30 dB | 0.00001610 |

A production Gate onset/release witness records 2,400 transported intervals
and eleven fractional writes. C58 charge, equivalent control and endpoint
gain are bit-identical with the correction enabled and disabled. The
resulting audio residual is -79.60 dBr: this is a subtle numerical correction,
not a new loudness calibration or an original-unit fidelity score. The test
also checks cold reset and identical audio with one- and 64-frame host blocks.

The interval occupies 28 bytes in each of the existing 32 contexts, plus 28
bytes per voice. With the simultaneous cutoff reconstruction, native engine
size is 585,672 bytes versus 569,672 at the pinned baseline; the existing
576 KiB Rack ceiling remains unchanged with 4,152 bytes of margin. Context
size is 180 bytes. There are no added callback allocations or unbounded
iterations. Ordinary samples need only arithmetic; a converter event adds
one prepared-current lookup for its intermediate knot.

Run the test executable with `--benchmark` separately, after other builds and
tests finish, for an interleaved same-binary six-voice Normal-grid CPU audit.
The benchmark is kept outside the functional CTest path so parallel tests
cannot silently turn timing contention into a performance claim. Initial
exploratory median times were 0.24867 to 0.25000 seconds (+0.54%); repeat under
the final combined build before treating those timings as release evidence.

Numerical convergence qualifies the implementation of the selected circuit.
Installed Tr20 parameters and original-unit audio remain separate evidence.
