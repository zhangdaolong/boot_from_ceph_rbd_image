# RBD extent mapping test vectors

These vectors exercise the pure byte-offset-to-object mapping implemented in `CephRbdLayout.c`. Extents stop at each stripe-unit boundary; the caller continues until the requested length is satisfied.

| Image size | Object size | Stripe unit | Stripe count | Image offset | Max length | Object number | Object offset | Extent length |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1048576 | 4194304 | 4194304 | 1 | 0 | 4096 | 0 | 0 | 4096 |
| 1048576 | 4194304 | 4194304 | 1 | 4194300 | 16 | 0 | 4194300 | 4 |
| 16777216 | 8388608 | 4194304 | 2 | 0 | 4194304 | 0 | 0 | 4194304 |
| 16777216 | 8388608 | 4194304 | 2 | 4194304 | 4194304 | 1 | 0 | 4194304 |
| 16777216 | 8388608 | 4194304 | 2 | 8388608 | 4194304 | 0 | 4194304 | 4194304 |
| 16777216 | 8388608 | 4194304 | 2 | 12582912 | 4194304 | 1 | 4194304 | 4194304 |
| 10000 | 4096 | 4096 | 1 | 8192 | 4096 | 2 | 0 | 1808 |

Invalid cases include zero image/object/stripe sizes, zero stripe count, object size not divisible by stripe unit, zero requested length, and offsets at or beyond image size. All must return an EFI error and must not modify a successful extent result.
