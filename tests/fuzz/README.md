# Fuzz harnesses

Parser hardening for data received from the network (SPEC §8, §14, §17).

| Harness | Target | Invariants |
|---|---|---|
| `fuzz_dns_message` | `dns::Message::parse` | no crash/UB; successful parses re-encode and re-parse |
| `fuzz_dns_name` | `dns::Name` (presentation + wire) | no crash/UB; normalization is idempotent; wire round-trip is stable |

Run on Linux (Clang + libFuzzer):

```bash
export VCPKG_ROOT=~/vcpkg
scripts/wsl-fuzz.sh 120 dns_message
scripts/wsl-fuzz.sh 120 dns_name
```

`tests/fuzz/corpus/*.bin` holds regression inputs found so far; the script seeds
each run's corpus from it. Every crash must end up here as a `.bin` file with a
comment in the commit message explaining the invariant that failed.

## Found so far

- `dns_message-multi-opt.bin` — a packet with two EDNS OPT records was
  half-consumed; re-encoding emitted the second OPT, which the parser then
  rejected. Fixed by rejecting >1 OPT and never emitting OPT from
  `Message::additional`.
- `dns_name-idn-casefold.bin` — `Aü` punycode-encoded with an uppercase ASCII
  letter, so re-parsing the presentation form produced a different name. Fixed
  by lowercasing ASCII before punycode.
