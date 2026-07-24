# Fan BPF design verification

- Sample rate: 10000 Hz
- Filter: fourth-order Butterworth band-pass (2 scipy prototype order)
- Cutoffs: 20 Hz to 100 Hz
- Runtime form: two float32 SOS, DF2T
- Maximum float64 pole radius: 0.992779398
- Maximum float32 pole radius: 0.992779413
- 33.33 to 76.67 Hz float32 gain range: -0.6433 to 0.0002 dB

## Float32 SOS coefficients

`[b0, b1, b2, a0, a1, a2]` using scipy's denominator convention.

```text
SOS 1: 0.0006098547019f, 0.001219709404f, 0.0006098547019f, 1f, -1.94198072f, 0.9449790716f
SOS 2: 1f, -2f, 1f, 1f, -1.985410213f, 0.9856109619f
```

## Frequency response

| Frequency (Hz) | Gain float64 | dB float64 | dB float32 |
|---:|---:|---:|---:|
| DC approximation | 0.00001600 | -95.916 | -95.918 |
| 5.000 | 0.04098970 | -27.747 | -27.748 |
| 10.000 | 0.17458068 | -15.160 | -15.161 |
| 20.000 | 0.70710678 | -3.010 | -3.011 |
| 30.000 | 0.97862857 | -0.188 | -0.187 |
| 33.333 | 0.99387806 | -0.053 | -0.053 |
| 50.000 | 0.99987846 | -0.001 | -0.001 |
| 76.667 | 0.92861038 | -0.643 | -0.643 |
| 80.000 | 0.90415041 | -0.875 | -0.875 |
| 100.000 | 0.70710678 | -3.010 | -3.010 |
| 150.000 | 0.32389436 | -9.792 | -9.792 |
| 300.000 | 0.07378007 | -22.641 | -22.641 |
| 1000.000 | 0.00601036 | -44.422 | -44.422 |
| 4500.000 | 0.00001586 | -95.995 | -95.995 |

## Group delay

| Frequency (Hz) | Samples | Milliseconds |
|---:|---:|---:|
| 20.000 | 168.791 | 16.879 |
| 30.000 | 105.054 | 10.505 |
| 33.333 | 86.456 | 8.646 |
| 50.000 | 51.416 | 5.142 |
| 76.667 | 45.517 | 4.552 |
| 80.000 | 44.460 | 4.446 |
| 100.000 | 33.780 | 3.378 |

The runtime C implementation uses `state1 = b1*x - a1*y + state2` and
`state2 = b2*x - a2*y`, matching scipy's SOS sign convention.
