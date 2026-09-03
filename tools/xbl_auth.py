"""Xbox LIVE sign-in, shared by the fetch_* tools.

Device-code against login.live.com using the public client id Microsoft's own
Xbox apps use: an eight-character code typed at microsoft.com/link, no
application registration, and nothing copied out of a browser.

An earlier version used the desktop redirect flow, which ends on a page reading
"Microsoft will never ask you to copy or share this URL." That warning is right
-- the URL carries a live authorization code -- so the redirect flow is gone.

The refresh token is cached in tools/.xbl_token.json (gitignored, it is a
credential). Delete that file to sign out.
"""
import json
import os
import time
from pathlib import Path

try:
    import requests
except ImportError:  # pragma: no cover
    raise SystemExit("this needs 'requests':  python -m pip install requests")

HERE = Path(__file__).resolve().parent
TOKEN_CACHE = HERE / ".xbl_token.json"

CLIENT_ID = "000000004C12AE6F"
CONNECT = "https://login.live.com/oauth20_connect.srf"
TOKEN = "https://login.live.com/oauth20_token.srf"
SCOPE = "Xboxlive.signin Xboxlive.offline_access"

XBL_USER = "https://user.auth.xboxlive.com/user/authenticate"
XSTS = "https://xsts.auth.xboxlive.com/xsts/authorize"


def _device_login() -> dict:
    r = requests.post(CONNECT, timeout=30, data={
        "client_id": CLIENT_ID, "scope": SCOPE, "response_type": "device_code"})
    if r.status_code != 200:
        raise SystemExit("could not start sign-in (%d): %s" % (r.status_code, r.text[:300]))
    d = r.json()
    print()
    print("  Go to:   %s" % d.get("verification_uri", "https://www.microsoft.com/link"))
    print("  Enter:   %s" % d["user_code"])
    print()
    print("  Sign in and approve. Nothing needs to be copied back here.")
    print("  Waiting...", end="", flush=True)
    try:
        import webbrowser
        webbrowser.open(d.get("verification_uri", "https://www.microsoft.com/link"))
    except Exception:
        pass

    deadline = time.time() + int(d.get("expires_in", 900))
    interval = int(d.get("interval", 5))
    while time.time() < deadline:
        time.sleep(interval)
        print(".", end="", flush=True)
        t = requests.post(TOKEN, timeout=30, data={
            "client_id": CLIENT_ID,
            "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
            "device_code": d["device_code"]})
        if t.status_code == 200:
            print(" signed in.")
            return t.json()
        try:
            err = t.json().get("error", "")
        except Exception:
            err = t.text[:120]
        if err == "slow_down":
            interval += 5
            continue
        if err == "authorization_pending":
            continue
        if err == "authorization_declined":
            raise SystemExit("\n  sign-in was declined")
        raise SystemExit("\n  sign-in failed: %s" % t.text[:300])
    raise SystemExit("\n  sign-in timed out")


def _refresh(refresh_token: str):
    t = requests.post(TOKEN, timeout=30, data={
        "client_id": CLIENT_ID, "refresh_token": refresh_token,
        "grant_type": "refresh_token", "scope": SCOPE})
    return t.json() if t.status_code == 200 else None


def sign_in(quiet: bool = False, relying_party: str = "http://xboxlive.com") -> tuple:
    """Returns (authorization_header, xuid), signing in only if it must.

    relying_party picks which service the ticket is good for. Nearly everything
    here wants the default; inventory.xboxlive.com is the exception and answers
    403 to it, because entitlements are licensing rather than social. Asking XSTS
    for "http://licensing.xboxlive.com" instead gets a ticket it accepts. Same
    account and the same sign-in either way -- only the audience differs.

    The refresh token rotates on every use, so the cache is rewritten each time;
    losing the new one means signing in again for nothing.
    """
    tok = None
    if TOKEN_CACHE.exists():
        try:
            cached = json.loads(TOKEN_CACHE.read_text())
            if cached.get("refresh_token"):
                tok = _refresh(cached["refresh_token"])
                if tok and not quiet:
                    print("  signed in from the cached token")
        except Exception:
            tok = None
    if not tok:
        tok = _device_login()
    if tok.get("refresh_token"):
        TOKEN_CACHE.write_text(json.dumps({"refresh_token": tok["refresh_token"], "live": True}))
        try:
            os.chmod(TOKEN_CACHE, 0o600)
        except Exception:
            pass

    # "d=" is what the ticket wants. The widespread claim that the live.com flow
    # takes the token bare is wrong, and being wrong about it costs a bare 401
    # with an empty body and nothing in it to go on.
    r = requests.post(XBL_USER, timeout=30, headers={"x-xbl-contract-version": "1"}, json={
        "Properties": {"AuthMethod": "RPS", "SiteName": "user.auth.xboxlive.com",
                       "RpsTicket": "d=" + tok["access_token"]},
        "RelyingParty": "http://auth.xboxlive.com", "TokenType": "JWT"})
    if r.status_code != 200:
        raise SystemExit("Xbox user auth failed (%d) %s"
                         % (r.status_code, r.headers.get("X-Err", r.text[:200])))
    user_token = r.json()["Token"]

    r = requests.post(XSTS, timeout=30, headers={"x-xbl-contract-version": "1"}, json={
        "Properties": {"SandboxId": "RETAIL", "UserTokens": [user_token]},
        "RelyingParty": relying_party, "TokenType": "JWT"})
    if r.status_code != 200:
        if "2148916233" in r.text:
            raise SystemExit("that Microsoft account has no Xbox profile on it")
        raise SystemExit("XSTS failed (%d): %s" % (r.status_code, r.text[:300]))
    d = r.json()
    claims = d["DisplayClaims"]["xui"][0]
    return "XBL3.0 x=%s;%s" % (claims["uhs"], d["Token"]), claims.get("xid")


def get(url: str, auth: str, contract: str = "2", **kw):
    """A GET with the headers every xboxlive.com service expects."""
    headers = {"Authorization": auth, "x-xbl-contract-version": contract,
               "Accept-Language": "en-US", "Accept": "application/json"}
    headers.update(kw.pop("headers", {}))
    return requests.get(url, timeout=kw.pop("timeout", 30), headers=headers, **kw)
