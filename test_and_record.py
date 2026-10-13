#!/usr/bin/env python3
"""Headless checks for the grokdesk simulator, demo mode and live mode.

Serves this directory, drives Chrome, writes screenshots/, and (if ffmpeg
is installed) records the kiosk auto-demo to demo.mp4.
"""
import asyncio, glob, json, os, shutil, socket, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from playwright.async_api import async_playwright

ROOT = os.path.dirname(os.path.abspath(__file__))
PORT = 8765
URL = f"http://127.0.0.1:{PORT}/index.html"
CHROME = os.environ.get("GROKDESK_CHROME", "/usr/bin/google-chrome")


def wait_port(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket() as s:
            s.settimeout(0.2)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.1)
    raise RuntimeError(f"server did not listen on {port}")


async def shot(page, name):
    path = os.path.join(ROOT, "screenshots", name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    await page.screenshot(path=path)
    print("screenshot", name)


async def demo(page):
    errs = []
    page.on("pageerror", lambda e: errs.append(str(e)))
    await page.goto(URL)
    await page.wait_for_timeout(400)
    tag = await page.inner_text("#mode-tag")
    badge = await page.inner_text("#mode-badge")
    assert tag == "SIMULATED DATA", tag
    assert badge == "SIMULATED", badge
    assert "SIMULATED" in await page.inner_text("body")
    checks = {
        "healthy": "DEPLOY HEALTHY",
        "failed": "DEPLOY FAILED",
        "explained": "migration 0042",
        "resolved": "rolled back",
    }
    for i, (state, txt) in enumerate(checks.items(), 1):
        await page.click(f'#controls button[data-state="{state}"]')
        if state == "explained":
            await page.wait_for_timeout(300)
            assert await page.get_attribute("#screen", "data-state") == "analyzing"
            await shot(page, "3a-analyzing.png")
        await page.wait_for_timeout(2200)
        got = await page.get_attribute("#screen", "data-state")
        body = await page.inner_text("#screen")
        ok = got == state and txt in body
        print(f"demo {state:10} state={got} contains {txt!r}: {ok}")
        assert ok, (state, got, body)
        fname = f"{i}-{state}.png" if state != "explained" else "3b-explained.png"
        await shot(page, fname)
    assert await page.inner_text("#mode-tag") == "SIMULATED DATA"
    await page.click("#auto")
    seen = []
    for _ in range(140):
        st = await page.get_attribute("#screen", "data-state")
        if not seen or seen[-1] != st:
            seen.append(st)
        await page.wait_for_timeout(100)
    if seen and seen[0] == "resolved":
        seen = seen[1:]
    print("auto demo sequence:", seen)
    assert seen == ["healthy", "failed", "analyzing", "explained", "resolved"], seen
    assert not errs, errs


async def live(page):
    with open(os.path.join(ROOT, "status.json"), encoding="utf-8") as f:
        root_doc = json.load(f)
    root_state = str(root_doc.get("state") or "")
    if root_state == "failed" and (root_doc.get("summary") or root_doc.get("fix")):
        root_visual = "explained"
    elif root_state not in ("healthy", "failed", "analyzing", "resolved"):
        root_visual = "error"
    else:
        root_visual = root_state
    root_txt = str(root_doc.get("summary") or root_doc.get("service") or "DEPLOY")
    cases = [
        ("?live", root_state, root_visual, root_txt, "live-root.png"),
        ("?live=./fixtures/failed.json", "failed", "explained", "migration 0042", "live-failed.png"),
        ("?live=./fixtures/failed-plain.json", "failed", "failed", "DEPLOY FAILED", "live-failed-plain.png"),
        ("?live=./fixtures/analyzing.json", "analyzing", "analyzing", "reading the failed rollout", "live-analyzing.png"),
        ("?live=./fixtures/resolved.json", "resolved", "resolved", "Rolled back to v2.14.0", "live-resolved.png"),
        ("?live=./fixtures/unknown.json", "exploded", "error", "UNKNOWN STATE", "live-unknown.png"),
        ("?live=./fixtures/missing.json", "error", "error", "STATUS UNREACHABLE", "live-missing.png"),
    ]
    for query, state, visual, txt, name in cases:
        errs = []
        page.on("pageerror", lambda e: errs.append(str(e)))
        await page.goto(URL + query)
        await page.wait_for_function(
            "() => document.getElementById('mode-tag').textContent === 'LIVE'"
        )
        # poll() is async; wait until the screen leaves the initial HTML.
        await page.wait_for_function(
            """(expected) => {
                const s = document.getElementById('screen');
                return s && s.dataset.state === expected;
            }""",
            arg=state,
            timeout=8000,
        )
        got = await page.get_attribute("#screen", "data-state")
        vis = await page.get_attribute("#screen", "data-visual")
        body = await page.inner_text("body")
        badge = await page.inner_text("#mode-badge")
        ok = got == state and vis == visual and txt in body and badge == "LIVE"
        print(f"live {name:22} state={got} visual={vis} badge={badge}: {ok}")
        assert ok, (query, got, vis, body[:500])
        assert "SIMULATED DATA" not in body
        assert "pipeline #4812" not in body
        if "failed.json" in query:
            assert "Rollback to v2.14.0" in body
            assert "Suggest:" in body
        await shot(page, name)
        assert not errs, errs


async def record(browser):
    rec = os.path.join(ROOT, ".rec")
    shutil.rmtree(rec, ignore_errors=True)
    os.makedirs(rec)
    ctx = await browser.new_context(
        viewport={"width": 1280, "height": 720},
        record_video_dir=rec,
        record_video_size={"width": 1280, "height": 720},
    )
    page = await ctx.new_page()
    await page.goto(URL + "?kiosk&autostart")
    await page.wait_for_timeout(13300)
    await ctx.close()
    vids = glob.glob(os.path.join(rec, "*.webm"))
    assert vids, "no recording"
    raw = os.path.join(rec, "raw.webm")
    shutil.move(vids[0], raw)
    if shutil.which("ffmpeg"):
        out = os.path.join(ROOT, "demo.mp4")
        subprocess.check_call([
            "ffmpeg", "-y", "-i", raw, "-ss", "0.4", "-t", "12.8",
            "-c:v", "libx264", "-crf", "18", "-pix_fmt", "yuv420p",
            "-r", "30", "-movflags", "+faststart", out,
        ])
        size = os.path.getsize(out)
        print(f"demo.mp4 {size} bytes")
        if size > 5 * 1024 * 1024:
            os.remove(out)
            print("demo.mp4 exceeded 5MB and was removed; raw video remains in .rec/")
    else:
        print("ffmpeg missing; left .rec/raw.webm")


VIEWPORTS = [(390, 844), (844, 390), (768, 1024), (1280, 720)]


async def assert_fits(page, label, expect_log):
    await page.wait_for_function(
        "() => document.getElementById('scr-title').textContent.includes('DEPLOY HEALTHY')"
    )
    metrics = await page.evaluate(
        """() => {
          const doc = document.documentElement;
          const title = document.getElementById('scr-title');
          const range = document.createRange();
          range.selectNodeContents(title);
          const box = range.getBoundingClientRect();
          const screen = document.getElementById('screen').getBoundingClientRect();
          const device = document.getElementById('device').getBoundingClientRect();
          const fully = box.width > 8 && box.height > 8
            && box.left >= -0.5 && box.top >= -0.5
            && box.right <= window.innerWidth + 0.5
            && box.bottom <= window.innerHeight + 0.5
            && box.left >= screen.left - 1
            && box.right <= screen.right + 1
            && box.top >= screen.top - 1
            && box.bottom <= screen.bottom + 1;
          return {
            scrollWidth: doc.scrollWidth,
            clientWidth: doc.clientWidth,
            bodyScroll: document.body.scrollWidth,
            bodyClient: document.body.clientWidth,
            text: title.textContent,
            fully,
            box: {l: box.left, r: box.right, t: box.top, b: box.bottom, w: box.width, h: box.height},
            screen: {l: screen.left, r: screen.right, w: screen.width, h: screen.height},
            device: {w: device.width, h: device.height, l: device.left, r: device.right, t: device.top, b: device.bottom},
            inner: [window.innerWidth, window.innerHeight],
            logDisplay: getComputedStyle(document.querySelector('.logpanel')).display,
            log: (() => {
              const r = document.querySelector('.logpanel').getBoundingClientRect();
              return {l: r.left, r: r.right, t: r.top, b: r.bottom, w: r.width, h: r.height};
            })(),
            viewportMeta: document.querySelector('meta[name=viewport]').content,
          };
        }"""
    )
    print(label, {k: metrics[k] for k in ("scrollWidth", "clientWidth", "text", "fully", "logDisplay", "device", "box")})
    assert "width=device-width" in metrics["viewportMeta"], metrics["viewportMeta"]
    assert metrics["scrollWidth"] <= metrics["clientWidth"] + 1, metrics
    assert metrics["bodyScroll"] <= metrics["bodyClient"] + 1, metrics
    assert metrics["text"] == "DEPLOY HEALTHY", metrics["text"]
    assert metrics["fully"], metrics
    dev = metrics["device"]
    assert dev["l"] >= -1 and dev["t"] >= -1, metrics
    assert dev["r"] <= metrics["inner"][0] + 1 and dev["b"] <= metrics["inner"][1] + 1, metrics
    if expect_log is True:
        assert metrics["logDisplay"] != "none", metrics
        log = metrics["log"]
        assert log["w"] > 40 and log["h"] > 40, metrics
        assert log["l"] >= -1 and log["t"] >= -1, metrics
        assert log["r"] <= metrics["inner"][0] + 1 and log["b"] <= metrics["inner"][1] + 1, metrics
    elif expect_log is False:
        assert metrics["logDisplay"] == "none", metrics


async def viewports(browser):
    for width, height in VIEWPORTS:
        for mode in ("demo", "live"):
            page = await browser.new_page(viewport={"width": width, "height": height})
            # Root status.json is the live document and changes. Layout checks
            # use a fixed healthy fixture so the headline assertion stays stable.
            query = "?live=./fixtures/healthy.json" if mode == "live" else ""
            await page.goto(URL + query)
            if mode == "live":
                await page.wait_for_function(
                    "() => document.getElementById('screen').dataset.state === 'healthy'"
                )
            narrow = width <= 900
            expect_log = False if (mode == "live" and narrow) else True
            await assert_fits(page, f"{mode} {width}x{height}", expect_log)
            await shot(page, f"viewport/{mode}-{width}x{height}.png")
            await page.close()
        # kiosk on a phone hides the log the same way live does
        if width <= 900:
            page = await browser.new_page(viewport={"width": width, "height": height})
            await page.goto(URL + "?kiosk")
            await assert_fits(page, f"kiosk {width}x{height}", False)
            await page.close()


async def cache_bust(browser):
    """A max-age=600 response must not pin ?live to the first body."""
    hits = []
    doc = {
        "state": "healthy",
        "service": "checkout-api",
        "summary": "Rollout complete. 3/3 pods ready.",
        "fix": "",
        "run_url": "https://example.test/1",
        "updated_at": "2026-10-13T09:13:40Z",
    }

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/status.json":
                hits.append({
                    "path": self.path,
                    "cache_control": self.headers.get("Cache-Control"),
                    "pragma": self.headers.get("Pragma"),
                })
                raw = json.dumps(doc).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Cache-Control", "max-age=600")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                return
            rel = (path.lstrip("/") or "index.html")
            file_path = os.path.join(ROOT, rel)
            if not os.path.isfile(file_path) or not os.path.realpath(file_path).startswith(ROOT):
                self.send_error(404)
                return
            ctype = "text/html"
            if rel.endswith(".js"):
                ctype = "text/javascript"
            elif rel.endswith(".css"):
                ctype = "text/css"
            data = open(file_path, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, fmt, *args):
            return

    httpd = ThreadingHTTPServer(("127.0.0.1", 8767), Handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    try:
        page = await browser.new_page(viewport={"width": 1280, "height": 720})
        await page.goto("http://127.0.0.1:8767/?live")
        await page.wait_for_function(
            "() => document.getElementById('screen').dataset.state === 'healthy'"
        )
        doc.update({
            "state": "failed",
            "service": "typerca",
            "summary": "Replay gate failed: supported rate 52% (62/120), needs 70%",
            "fix": "New test tests/test_grokdesk_demo.py sets the bar too high; fix or drop it",
            "run_url": "https://github.com/mingleiw/typerca/actions/runs/38377473690",
            "updated_at": "2026-10-13T09:47:00Z",
        })
        await page.evaluate("() => window.DeskSim.poll()")
        await page.wait_for_function(
            "() => document.getElementById('screen').dataset.state === 'failed'"
        )
        screen = await page.inner_text("#screen")
        assert "52%" in screen and "Suggest:" in screen, screen
        assert len(hits) >= 2, hits
        stamps = []
        for hit in hits:
            assert "t=" in hit["path"], hit
            assert "no-cache" in (hit["cache_control"] or "").lower(), hit
            assert "no-cache" in (hit["pragma"] or "").lower(), hit
            stamps.append(hit["path"].split("t=", 1)[1])
        assert len(set(stamps)) == len(stamps), hits
        print("cache bust ok", hits)
        await page.close()
    finally:
        httpd.shutdown()


async def main():
    server = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(PORT), "--bind", "127.0.0.1"],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        wait_port(PORT)
        async with async_playwright() as p:
            browser = await p.chromium.launch(
                executable_path=CHROME,
                args=["--no-sandbox", "--disable-dev-shm-usage", "--headless=new"],
            )
            await cache_bust(browser)
            page = await browser.new_page(viewport={"width": 1280, "height": 720})
            await demo(page)
            await live(page)
            await page.close()
            await viewports(browser)
            await browser.close()
            if os.environ.get("GROKDESK_SKIP_RECORD") == "1":
                return
            browser = await p.chromium.launch(
                executable_path=CHROME,
                args=["--no-sandbox", "--disable-dev-shm-usage", "--headless=new"],
            )
            await record(browser)
            await browser.close()
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()


if __name__ == "__main__":
    asyncio.run(main())
