# tools/ — host side

Everything here is standard-library Python 3.9+ plus `pyserial` for the real
device. Nothing talks to the cloud.

| File | Role |
|---|---|
| `dashboard_server.py` | serial link, run recorder, replay driver, localhost dashboard + JSON API |
| `serial_protocol.py` | framed packet encode/parse, CRC-16, sequence-gap tracking (mirrors `src/telemetry_protocol.h`) |
| `wav_writer.py` | PCM → WAV with gap-filling and completeness flags |
| `dsp_reference.py` | bit-exact Python mirror of the firmware's mix and delay-and-sum, used to rebuild and verify processed paths |
| `run_storage.py` | one folder per run: metadata JSON, summary CSV, events JSONL, timeseries CSV |
| `static/` | the dashboard page (vanilla HTML/JS/CSS) |
| `tests/test_host.py` | unit tests; `make -C ../tests_host` first for the firmware cross-check vectors |

```bash
pip3 install -r tools/requirements.txt
python3 tools/dashboard_server.py --port /dev/tty.usbmodem<ID> --baud 1000000 --web-port 5000
python3 tools/dashboard_server.py --simulate      # synthetic device, no hardware
python3 -m unittest discover -s tools/tests -v
```

HTTP API (all JSON): `GET /api/state`, `GET /api/runs`, `GET /api/runs/<dir>`,
`GET /api/runs/<dir>/file/<name>`, `GET /api/runs/<dir>/report`,
`GET /api/compare?a=<dir>&b=<dir>`, `POST /api/command {line}`,
`POST /api/start {...}`, `POST /api/stop`, `POST /api/replay {run_dir, paths}`.
