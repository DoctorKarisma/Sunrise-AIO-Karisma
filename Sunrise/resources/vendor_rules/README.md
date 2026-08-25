# Vendor rule files

Reference copies of the authored rule files that drive the vendor behaviours, exactly as run
for the in-game verification. Without them most of the vendor code is inert: no catalog file
means only the head of the vendor index resolves (the Drifter is row 195 and never will), and
no bounty, exchange or substitution file means those behaviours never trigger.

Install them to `bin\x64\Sunrise\` beside `settings.json`. They are re-read on every use, so
editing one takes effect without a relaunch or rebuild.

| file | drives | keyed by |
|---|---|---|
| `vendor_catalog.txt` | which vendor definitions are published | vendor definition hash |
| `vendor_item_substitute.txt` | what a placeholder row really grants | item definition hash |
| `vendor_bounty_roll.txt` | the repeatable-bounty pools | vendor hash + trigger category |
| `vendor_exchange.txt` | recycle rows: cost and payouts | vendor hash + sale row |

Each file documents its own format and the reasoning in its header comments. Hashes are the
item and vendor definition hashes the manifest names; a hash this build does not carry is
skipped (bounty pools) or logged and refused (the rest), so rules authored from a newer
manifest degrade rather than fail whole.
