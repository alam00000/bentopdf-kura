# Kura colour profiles

Made by Kura from published formulas by `pdfa-engine/scripts/gen_icc.py`. No rights reserved: free to use, copy and embed.

| Profile | Built from |
|---|---|
| Dot Gain 10%, 15%, 20%, 25%, 30% | lightness = max(x^γ, x/16), γ = log(0.5 − gain) / log(0.5) |
| Gray Gamma 1.8, Gray Gamma 2.2 | a single gamma, D65 white |
| ProPhoto RGB | ROMM RGB primaries and gamma 1.8 (ISO 22028-2) |
| Lab D50, PCS XYZ, Flat XYZ | pass-through tables in the profile connection space |
| Black & White | threshold to black or white |

The colour data matches the widely installed profiles of the same names, so conversions give the same results.
