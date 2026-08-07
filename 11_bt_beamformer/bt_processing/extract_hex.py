import re

def extract_hex_from_log(log_path, out_path):
    hex_tokens = []

    with open(log_path, 'r') as f:
        for line in f:
            if "Updated Value of Characteristic 6E400003" in line:
                # extract everything after "to "
                m = re.search(r"to (.+)", line)
                if not m:
                    continue
                raw = m.group(1).strip()
                # remove trailing . if present
                if raw.endswith('.'):
                    raw = raw[:-1]
                # split into tokens
                hex_tokens.extend(raw.split())

    with open(out_path, 'w') as out:
        out.write(' '.join(hex_tokens))

    print(f"Wrote combined hex to {out_path}")

if __name__ == "__main__":
    extract_hex_from_log("nrf_ble_packet_log.txt", "ble_hex.txt")