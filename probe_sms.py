import requests
import hashlib
import re
import sys

# --- Configuration ---
ROUTER_IP = "192.168.100.10"
USERNAME = "admin"
PASSWORD = "Leda69+1"

BASE_URL = f"http://{ROUTER_IP}"
LOGIN_PAGE = f"{BASE_URL}/cgi-bin/luci/"
USER_AGENT = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36"

session = requests.Session()
session.headers.update({"User-Agent": USER_AGENT})

def get_token(html, name):
    match = re.search(f'name="{name}".*?value="(.*?)"', html, re.IGNORECASE | re.DOTALL)
    if match: return match.group(1)
    match = re.search(f'value="(.*?)".*?name="{name}"', html, re.IGNORECASE | re.DOTALL)
    return match.group(1) if match else ""

r1 = session.get(LOGIN_PAGE)
csrf, salt, token = get_token(r1.text, "_csrf"), get_token(r1.text, "salt"), get_token(r1.text, "token")
zonename, timeclock = get_token(r1.text, "zonename"), get_token(r1.text, "timeclock")

h1 = hashlib.sha256((PASSWORD + salt).encode()).hexdigest()
final_hash = hashlib.sha256((h1 + token).encode()).hexdigest()

post_data = {
    "_csrf": csrf, "token": token, "salt": salt,
    "zonename": zonename, "timeclock": timeclock,
    "luci_language": "auto", "luci_username": USERNAME,
    "luci_password": final_hash
}

r2 = session.post(LOGIN_PAGE, data=post_data, headers={"Referer": LOGIN_PAGE, "Origin": BASE_URL}, allow_redirects=False)

stok = ""
location = r2.headers.get("Location", "")
if ";stok=" in location:
    stok = location.split(";stok=")[1].split("/")[0]

if r2.status_code not in [200, 301, 302]:
    print(f"Login failed: {r2.status_code}")
    exit(1)

def build_url(path):
    return f"{BASE_URL}/cgi-bin/luci/;stok={stok}{path}" if stok else f"{BASE_URL}/cgi-bin/luci{path}"

# Detailed Read Check for BOTH Inbox and Sent
for box in ["rec", "sto"]:
    url_list = build_url(f"/admin/network/gcom/sms/smslist?smsbox={box}&iface=4g")
    r_list = session.get(url_list, headers={"Referer": build_url("/admin/network/gcom/sms?iface=4g")})
    
    print(f"\n--- BOX: {box} ---")
    
    # Extract first row detail link
    # The onclick is formatted like: cbi_show_modal(this, "/path", "query")
    match = re.search(r'onclick=\'cbi_show_modal\(this, "(.*?)", "(.*?)"\)\'', r_list.text)
    if match:
        detail_url = f"{BASE_URL}{match.group(1)}?{match.group(2)}"
        print(f"[*] Fetching details from: {detail_url}")
        r_detail = session.get(detail_url)
        
        # Look for labels and values
        # They are usually in <label> and <p class="form-control-static">
        print("[DETAILS CONTENT]")
        labels = re.findall(r'<label.*?>(.*?)</label>', r_detail.text)
        values = re.findall(r'<p class="form-control-static.*?>(.*?)</p>', r_detail.text, re.DOTALL)
        
        for i in range(min(len(labels), len(values))):
            lbl = re.sub('<[^<]+?>', '', labels[i]).strip()
            val = re.sub('<[^<]+?>', '', values[i]).strip()
            print(f"  {lbl}: {val}")
        
    else:
        print("  (No messages found)")

print("\n--- DONE ---")
