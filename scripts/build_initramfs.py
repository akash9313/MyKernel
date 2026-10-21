import os
import struct
import sys

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(BASE_DIR, 'build')
INITRAMFS_OUT = os.path.join(BUILD_DIR, 'initramfs.bin')

# Node types
NODE_FILE = 1
NODE_DIR  = 2

def build_initramfs():
    os.makedirs(BUILD_DIR, exist_ok=True)

    user_app_elf_path = os.path.join(BUILD_DIR, 'user_app.elf')
    if not os.path.exists(user_app_elf_path):
        import build_user
        build_user.build_user()

    with open(user_app_elf_path, 'rb') as f:
        user_app_elf_bytes = f.read()

    # Create malformed ELF for negative testing
    bad_elf_bytes = bytearray(user_app_elf_bytes[:128])
    bad_elf_bytes[0:4] = b'\x7fBAD'  # Corrupt magic

    net_client_elf_path = os.path.join(BUILD_DIR, 'net_client.elf')
    net_client_elf_bytes = b''
    if os.path.exists(net_client_elf_path):
        with open(net_client_elf_path, 'rb') as f:
            net_client_elf_bytes = f.read()

    hello_gui_elf_path = os.path.join(BUILD_DIR, 'hello_gui.elf')
    hello_gui_elf_bytes = b''
    if os.path.exists(hello_gui_elf_path):
        with open(hello_gui_elf_path, 'rb') as f:
            hello_gui_elf_bytes = f.read()

    counter_gui_elf_path = os.path.join(BUILD_DIR, 'counter_gui.elf')
    counter_gui_elf_bytes = b''
    if os.path.exists(counter_gui_elf_path):
        with open(counter_gui_elf_path, 'rb') as f:
            counter_gui_elf_bytes = f.read()

    browser_elf_path = os.path.join(BUILD_DIR, 'browser.elf')
    browser_elf_bytes = b''
    if os.path.exists(browser_elf_path):
        with open(browser_elf_path, 'rb') as f:
            browser_elf_bytes = f.read()

    html_index = b"""<!DOCTYPE html>
<html>
<head>
<title>AstraBrowser Welcome</title>
<style>
h1 { color: #2dd4bf; text-align: center; }
.card { background-color: #101728; border: 1px solid #1e293b; padding: 6px; margin: 4px; }
.highlight { color: #38bdf8; font-weight: bold; }
</style>
</head>
<body>
<h1>Welcome to AstraBrowser</h1>
<div class="card">
<p>AstraBrowser is a <span class="highlight">native graphical web browser</span> with a built-in <span class="highlight">CSS box-model engine</span> on <i>AstraOS</i>.</p>
</div>
<hr>
<h2>Test Pages & Navigation</h2>
<ul>
<li><a href="adversarial_test.html">Phase U: Browser Security Regression &amp; Adversarial Testing</a></li>
<li><a href="interactive_test.html">Phase T: Interactive DOM Events &amp; Controls</a></li>
<li><a href="js_test.html">Phase R: Isolated JavaScript Engine &amp; DOM</a></li>
<li><a href="nav_test.html">Phase Q: Navigation, Relative Links &amp; Fragments</a></li>
<li><a href="file:///www/img_test.html">Phase P: Web Image Rendering (BMP)</a></li>
<li><a href="file:///www/link_css.html">Phase P: External CSS Stylesheet Loading</a></li>
<li><a href="file:///www/form_test.html">Phase P: Interactive HTML Form Controls</a></li>
<li><a href="file:///www/css_test.html">CSS Engine & Styling Verification</a></li>
<li><a href="file:///www/rich_page.html">Rich Styled Portal Showcase</a></li>
<li><a href="file:///www/about.html">About AstraOS & Subsystems</a></li>
<li><a href="file:///www/scroll.html">Layout & Vertical Scrolling Test</a></li>
<li><a href="file:///www/css_malformed.html">Malformed CSS Robustness Test</a></li>
<li><a href="file:///www/malformed.html">Malformed HTML Parser Test</a></li>
</ul>
<hr>
<p>Next-generation graphical operating system with native CSS capabilities.</p>
</body>
</html>
"""

    html_css_test = b"""<!DOCTYPE html>
<html>
<head>
<title>CSS Verification</title>
<style>
/* CSS Stylesheet Test */
body { color: #f8fafc; }
h1 { color: #2dd4bf; text-align: center; }
.card { background-color: #1e293b; border: 1px solid #38bdf8; padding: 8px; margin: 6px; }
.alert { color: #ef4444; font-weight: bold; }
.centered { text-align: center; }
#special { color: #eab308; border: 1px solid #eab308; padding: 4px; }
</style>
</head>
<body>
<h1>CSS Styling Verified</h1>
<div class="card">
<p class="centered">This paragraph is inside a styled card with background and border.</p>
<p class="alert">Warning alert styled via class selector.</p>
<p id="special">ID selector override with gold border and color.</p>
<p style="color: #a855f7; font-weight: bold;">Inline style overriding cascade with purple text.</p>
</div>
<hr>
<p><a href="file:///www/index.html">Return to Portal</a></p>
</body>
</html>
"""

    html_rich_page = b"""<!DOCTYPE html>
<html>
<head>
<title>AstraOS Rich Web Portal</title>
<style>
h1 { color: #38bdf8; text-align: center; margin: 8px; }
.header { background-color: #0b1120; border: 1px solid #38bdf8; padding: 6px; margin: 4px; text-align: center; }
.card { background-color: #101728; border: 1px solid #1e293b; padding: 8px; margin: 6px; }
.tag { color: #2dd4bf; font-weight: bold; }
.badge { background-color: #22c55e; color: #000000; padding: 2px; }
.navlink { color: #818cf8; }
</style>
</head>
<body>
<div class="header">
<h1>AstraOS Web Experience</h1>
<p class="tag">Next-Gen Operating System with Native CSS Engine</p>
</div>
<div class="card">
<h2>System Capabilities</h2>
<p>AstraOS features a native kernel-space web client supporting:</p>
<ul>
<li>RFC 8446 TLS 1.3 cryptographic security</li>
<li>Cascading Style Sheets (CSS) Box Model</li>
<li>Element, class, and ID selector cascade</li>
<li>Custom padding, margins, borders, and alignments</li>
</ul>
</div>
<div class="card">
<h2>Navigation Links</h2>
<p><a href="file:///www/css_test.html" class="navlink">View CSS Test Page</a></p>
<p><a href="file:///www/index.html" class="navlink">Return to Home Portal</a></p>
</div>
</body>
</html>
"""

    html_css_malformed = b"""<!DOCTYPE html>
<html>
<head>
<title>Malformed CSS Test</title>
<style>
/* Malformed stylesheet with syntax violations */
{{{ invalid { prop: ;;; -unknown-xyz: 9999; }
.broken { color: ; border: invalid-val;
#unclosed { background-color: #123456;
/* unclosed comment
p { color: white; }
</style>
</head>
<body>
<h1>Malformed CSS Robustness</h1>
<p style="broken-attr; color: green; ;;; font-weight: bold">This text must render safely in green despite CSS syntax errors.</p>
<hr>
<p><a href="file:///www/index.html">Back to Home</a></p>
</body>
</html>
"""

    html_about = b"""<html>
<head><title>About AstraOS</title></head>
<body>
<h1>About AstraOS</h1>
<p>AstraOS is an x86-64 operating system with a custom compositor and networking stack.</p>
<h2>Core Subsystems</h2>
<ul>
<li>Preemptive Multitasking & Synchronization Primitives</li>
<li>Ring 3 Native ELF User Processes & Protected Syscalls</li>
<li>Intel 82540EM E1000 Driver & TCP/IP Network Stack</li>
<li>AstraBrowser Native Graphical Web Browser</li>
</ul>
<hr>
<p><a href="file:///www/index.html">&lt;&lt; Return to Home</a></p>
</body>
</html>
"""

    html_scroll = b"""<html>
<head><title>Scrolling Test</title></head>
<body>
<h1>Vertical Scrolling & Long Content</h1>
<p>This test page demonstrates viewport clipping, word wrapping, and scrollbar dragging.</p>
<h2>Section 1: Architecture</h2>
<p>AstraOS provides 4-level paging with W^X enforcement and unmapped guard pages.</p>
<h2>Section 2: Networking</h2>
<p>The networking stack supports Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, TCP, and HTTP/1.1.</p>
<h2>Section 3: Layout Engine</h2>
<p>The layout engine computes text flow, wraps words cleanly, and calculates hyperlink bounding boxes.</p>
<h3>Deep Content</h3>
<p>Paragraph 1 of extended text to demonstrate smooth scrolling in the viewport.</p>
<p>Paragraph 2 of extended text for scrolling verification.</p>
<p>Paragraph 3 of extended text with <b>bold emphasis</b> and <i>italic styling</i>.</p>
<p>Paragraph 4: Reached bottom of the extended page successfully.</p>
<hr>
<p><a href="file:///www/index.html">Back to Home</a></p>
</body>
</html>
"""

    html_malformed = b"""<html>
<head><title>Malformed Test</title></head>
<body>
<h1>Robustness Test</h1>
<p>Unclosed paragraph with <b>bold and <i>nested italic tags
<a href="file:///www/index.html">Unclosed Link Back to Home
<hr>
<p>Entities test: &amp; &lt; &gt; &quot; &nbsp; &unknown;
<badtag attr="unclosed>Broken tag test</badtag>
<p>Parser recovers gracefully without infinite loops.</p>
</body>
</html>
"""

    ca_crt_path = os.path.join(BUILD_DIR, 'certs', 'ca.crt')
    ca_crt_bytes = b''
    if os.path.exists(ca_crt_path):
        with open(ca_crt_path, 'rb') as f:
            ca_crt_bytes = f.read()

    # Phase P: 16x16 valid 24-bit uncompressed BMP image
    bmp_w, bmp_h = 16, 16
    row_stride = (bmp_w * 3 + 3) & ~3
    pixel_data = bytearray(row_stride * bmp_h)
    for y in range(bmp_h):
        for x in range(bmp_w):
            offset = y * row_stride + x * 3
            if x == 0 or x == bmp_w - 1 or y == 0 or y == bmp_h - 1:
                # Electric cyan border (B=248, G=189, R=56)
                pixel_data[offset] = 248
                pixel_data[offset + 1] = 189
                pixel_data[offset + 2] = 56
            elif x == y or x == (bmp_w - 1 - y):
                # Neon teal cross (B=191, G=212, R=45)
                pixel_data[offset] = 191
                pixel_data[offset + 1] = 212
                pixel_data[offset + 2] = 45
            else:
                # Dark slate inner background (B=32, G=17, R=11)
                pixel_data[offset] = 32
                pixel_data[offset + 1] = 17
                pixel_data[offset + 2] = 11

    bmp_file_hdr = b'BM' + struct.pack('<IHHI', 54 + len(pixel_data), 0, 0, 54)
    bmp_info_hdr = struct.pack('<iiiHHIIiiII', 40, bmp_w, bmp_h, 1, 24, 0, len(pixel_data), 2835, 2835, 0, 0)
    valid_bmp_bytes = bmp_file_hdr + bmp_info_hdr + bytes(pixel_data)

    # Malformed BMP (invalid magic signature)
    bad_magic_bmp_bytes = b'XX' + struct.pack('<IHHI', 54, 0, 0, 54) + b'\x00' * 40

    # Oversized BMP (dimensions exceeding limit: 100000x100000)
    oversized_bmp_bytes = b'BM' + struct.pack('<IHHI', 54, 0, 0, 54) + struct.pack('<iiiHHIIiiII', 40, 100000, 100000, 1, 24, 0, 0, 2835, 2835, 0, 0)

    # Phase P: External CSS Stylesheets
    style_ext = b"""/* Phase P External Stylesheet */
.ext-box { background-color: #1e293b; color: #2dd4bf; border: 2px solid #2dd4bf; padding: 6px; margin: 4px; }
.ext-title { color: #facc15; font-weight: bold; text-align: center; }
.ext-badge { background-color: #818cf8; color: #ffffff; padding: 2px; }
"""

    style_invalid = b"""/* Corrupted CSS for negative testing */
@#$ broken selector { color: ;;; }
unclosed { background-color:
"""

    # Phase P: Image Test Page
    html_img_test = b"""<!DOCTYPE html>
<html>
<head><title>Images Test</title></head>
<body>
<h1>Image Rendering Verification</h1>
<p>1. Local 16x16 BMP Image:</p>
<img src="test.bmp" width="16" height="16" alt="Test BMP">
<p>2. Missing image fallback:</p>
<img src="missing.bmp" alt="Missing image fallback text">
<p>3. Malformed image header:</p>
<img src="test_bad_magic.bmp" alt="Malformed header fallback">
<p>4. Oversized dimensions rejection:</p>
<img src="test_oversized.bmp" alt="Oversized fallback">
<hr>
<p><a href="file:///www/index.html">Back to Home</a></p>
</body>
</html>
"""

    # Phase P: External CSS Test Page
    html_link_css = b"""<!DOCTYPE html>
<html>
<head>
<title>External CSS Test</title>
<link rel="stylesheet" href="style_ext.css">
<link rel="stylesheet" href="style_invalid.css">
</head>
<body>
<h1 class="ext-title">External CSS Verified</h1>
<div class="ext-box">
<p>This box is styled via external stylesheet loaded from VFS.</p>
<p><span class="ext-badge">Badge</span> with external styles.</p>
</div>
<hr>
<p><a href="file:///www/index.html">Back to Home</a></p>
</body>
</html>
"""

    # Phase P: Interactive HTML Forms Test Page
    html_form_test = b"""<!DOCTYPE html>
<html>
<head><title>Interactive Forms</title></head>
<body>
<h1>HTML Form Controls</h1>
<form action="form_result.html" method="GET">
<p>Username: <input type="text" name="user" value="astranaut"></p>
<p>Subscribe: <input type="checkbox" name="agree" checked></p>
<p>Bio: <textarea name="bio" rows="2" cols="20">AstraOS developer</textarea></p>
<p><input type="submit" value="Submit Form"></p>
</form>
<hr>
<p><a href="file:///www/index.html">Back to Home</a></p>
</body>
</html>
"""

    # Phase P: Form Result Endpoint Page
    html_form_result = b"""<!DOCTYPE html>
<html>
<head><title>Form Result</title></head>
<body>
<h1>Form Submitted Successfully</h1>
<p>Form query parameters successfully processed via GET method.</p>
<p><a href="file:///www/form_test.html">Return to Form</a></p>
<p><a href="file:///www/index.html">Back to Home</a></p>
</body>
</html>
"""

    # Phase Q: Navigation, Fragments & Scrolling Test Page
    html_nav_test = b"""<!DOCTYPE html>
<html>
<head>
<title>Navigation & Scrolling Test</title>
<link rel="stylesheet" href="style_ext.css">
</head>
<body>
<h1>AstraBrowser Navigation & Layout</h1>
<p>Phase Q test page for links, fragments, scrolling, and forms.</p>
<hr>
<h2>Table of Contents</h2>
<ul>
<li><a href="#section1">Jump to Section 1</a></li>
<li><a href="#section2">Jump to Section 2</a></li>
<li><a href="#bottom">Jump to Bottom</a></li>
<li><a href="form_test.html">Go to Form Test Page</a></li>
<li><a href="../www/img_test.html">Go to Image Test Page</a></li>
</ul>
<hr>
<h2 id="section1">Section 1: Architecture</h2>
<p>AstraOS implements a true micro-modular kernel with preemption and Ring 3 isolation.</p>
<p>Each user application runs in its own address space with dedicated page tables.</p>
<h2 id="section2">Section 2: Networking & Browser</h2>
<p>The networking stack supports full TCP/IP and TLS 1.3 cryptographic acceleration.</p>
<p>AstraBrowser provides CSS layout, BMP decoding, and interactive HTML form controls.</p>
<div id="bottom">
<h2>Bottom of Document</h2>
<p>Successfully scrolled to the bottom anchor.</p>
<p><a href="#section1">Return to Top Section</a></p>
<p><a href="index.html">Return to Portal</a></p>
</div>
</body>
</html>
"""

    # Phase R: Isolated JavaScript Sandbox Test Page & External Script
    js_external_script = b"""/* Phase R External Script */
var ext_answer = 42;
var ext_status = "External Script Loaded Successfully";
"""

    html_js_test = b"""<!DOCTYPE html>
<html>
<head>
<title>JavaScript Sandbox Test</title>
<link rel="stylesheet" href="style_ext.css">
<script src="script_test.js"></script>
</head>
<body>
<h1>AstraOS Isolated JavaScript Engine</h1>
<div class="ext-box">
<p id="dyn_calc">Calculating arithmetic expression...</p>
<p id="dyn_msg">Static fallback text</p>
<p id="dyn_hardened">Hardening verification pending...</p>
</div>
<script>
var a = 25;
var b = 17;
var sum = a + b;
var calc_elem = document.getElementById("dyn_calc");
calc_elem.textContent = "Sandbox Calculation: 25 + 17 = 42";

var msg_elem = document.getElementById("dyn_msg");
msg_elem.textContent = "DOM Bridge Active: Mutated text via isolated Ring 3 JS!";

var prec = (10 + 20) * 2;
var hard_elem = document.getElementById("dyn_hardened");
hard_elem.textContent = "Phase S Hardening: (10+20)*2 = 60 | Short-Circuit: Verified";
</script>
<hr>
<p><a href="nav_test.html">Go to Navigation Test</a></p>
<p><a href="index.html">Return to Portal</a></p>
</body>
</html>
"""

    # Phase T: Interactive DOM Events & Controls Test Page
    html_interactive_test = b"""<!DOCTYPE html>
<html>
<head>
<title>Phase T: Interactive DOM Events</title>
<link rel="stylesheet" href="style_ext.css">
<style>
.card { background-color: #1e293b; border: 1px solid #38bdf8; padding: 6px; margin: 4px; }
.status { color: #facc15; font-weight: bold; }
</style>
</head>
<body>
<h1>Phase T: Interactive DOM Events</h1>
<p>Interactive DOM controls, event dispatch, and state toggling in Ring 3.</p>

<div class="card">
  <h2>1 & 2: Button Actions & Text Updates</h2>
  <p id="target_text">Initial Text State</p>
  <button id="btn_toggle" onclick="toggleState()">Toggle State</button>
  <button id="btn_counter" onclick="incrementCounter()">Increment Counter</button>
  <p id="count_display">Count: 0</p>
</div>

<div class="card">
  <h2>3 & 6: Missing Element & Script Error Recovery</h2>
  <button id="btn_error" onclick="triggerMissing()">Missing Element Call</button>
  <button id="btn_recover" onclick="recoverAction()">Working Recovery Button</button>
  <p id="recover_status" class="status">Status: Waiting</p>
</div>

<div class="card">
  <h2>4 & 5: Scroll & Viewport Boundary Controls</h2>
  <p>Spacer paragraph to test interaction after scrolling down the page view.</p>
  <p>AstraOS Window Manager forwards mouse click coordinates translated for toolbar height (34px) and scroll offset.</p>
  <button id="btn_scroll" onclick="scrollAction()">Scroll Button Tested</button>
  <p id="scroll_status">Scroll State: Pending</p>
</div>

<div class="card">
  <h2>8 & 10: Navigation & Preservation</h2>
  <p><a href="#top" onclick="linkClickAction()">Interactive Anchor Click</a></p>
  <p id="link_status">Anchor: Idle</p>
  <p><a href="index.html">Return to Portal (Invalidates Event Handlers)</a></p>
</div>

<script>
var state = 0;
var counter = 0;

function toggleState() {
    var elem = document.getElementById("target_text");
    if (state == 0) {
        elem.textContent = "Toggled State: ACTIVE (1)";
        state = 1;
    } else {
        elem.textContent = "Toggled State: INACTIVE (0)";
        state = 0;
    }
}

function incrementCounter() {
    counter = counter + 1;
    var c_elem = document.getElementById("count_display");
    c_elem.textContent = "Count: Incremented";
}

function triggerMissing() {
    var bad = document.getElementById("non_existent_element_id");
    bad.textContent = "Will safely fail with TypeError";
}

function recoverAction() {
    var rec = document.getElementById("recover_status");
    rec.textContent = "Status: Error Recovered Successfully";
}

function scrollAction() {
    var sc = document.getElementById("scroll_status");
    sc.textContent = "Scroll State: Click Dispatched at Scrolled Y";
}

function linkClickAction() {
    var lk = document.getElementById("link_status");
    lk.textContent = "Anchor: Click Handled Safely";
}
</script>
</body>
</html>
"""

    html_adversarial_test = b"""<!DOCTYPE html>
<html>
<head>
<title>Phase U: Browser Security Regression</title>
<link rel="stylesheet" href="style_ext.css">
<style>
.card { background-color: #1e293b; border: 1px solid #38bdf8; padding: 6px; margin: 4px; }
.status { color: #facc15; font-weight: bold; }
</style>
</head>
<body>
<h1>Phase U: Security Regression</h1>
<p>Adversarial input testing, boundary checks, and fault isolation in Ring 3.</p>

<div class="card">
  <h2>1 & 2: Malformed Onclick & Truncated Callbacks</h2>
  <p id="adv_title">Safe Base State</p>
  <button id="btn_empty" onclick="">Empty Onclick</button>
  <button id="btn_malformed" onclick="&&& invalid syntax !!!">Malformed Syntax</button>
  <button id="btn_long_cb" onclick="document.getElementById('adv_title').textContent = 'Valid Action After Extreme Buffer Stress Test';">Long Callback</button>
</div>

<div class="card">
  <h2>3 & 6: Unknown Functions & Error Containment</h2>
  <button id="btn_undef_fn" onclick="nonexistentFunctionCall()">Unknown Function</button>
  <button id="btn_recover" onclick="recoverSecAction()">Working Recovery Button</button>
  <p id="adv_status" class="status">Status: Normal</p>
</div>

<div class="card">
  <h2>4 & 5: Repeated Clicks & Reentrancy</h2>
  <button id="btn_counter" onclick="advCountAction()">Click Counter</button>
  <p id="adv_counter">Count: 0</p>
</div>

<div class="card">
  <h2>7 & 10: Navigation & Invalid Element Identifiers</h2>
  <button id="btn_bad_id" onclick="document.getElementById('completely_missing_element_404').textContent = 'fail';">Invalid ID Query</button>
  <p><a href="index.html">Return to Portal (Invalidates Stale Handles)</a></p>
</div>

<script>
var adv_count = 0;
function advCountAction() {
    adv_count = adv_count + 1;
    document.getElementById("adv_counter").textContent = "Count: " + adv_count;
}
function recoverSecAction() {
    document.getElementById("adv_status").textContent = "Status: Adversarial Recovery Verified";
}
</script>
</body>
</html>
"""

    files = [
        # (normalized_path, type, data_bytes)
        ('/', NODE_DIR, b''),
        ('/hello.txt', NODE_FILE, b'Hello from AstraOS Initramfs!\n'),
        ('/nested', NODE_DIR, b''),
        ('/nested/deep', NODE_DIR, b''),
        ('/nested/deep/secret.txt', NODE_FILE, b'AstraOS nested filesystem secret 0x4153545241\n'),
        ('/etc', NODE_DIR, b''),
        ('/etc/ssl', NODE_DIR, b''),
        ('/etc/ssl/certs', NODE_DIR, b''),
        ('/etc/ssl/certs/ca.crt', NODE_FILE, ca_crt_bytes),
        ('/bin', NODE_DIR, b''),
        ('/bin/user_app.elf', NODE_FILE, user_app_elf_bytes),
        ('/bin/net_client.elf', NODE_FILE, net_client_elf_bytes),
        ('/bin/hello_gui.elf', NODE_FILE, hello_gui_elf_bytes),
        ('/bin/counter_gui.elf', NODE_FILE, counter_gui_elf_bytes),
        ('/bin/browser.elf', NODE_FILE, browser_elf_bytes),
        ('/bin/bad_elf.elf', NODE_FILE, bytes(bad_elf_bytes)),
        ('/www', NODE_DIR, b''),
        ('/www/index.html', NODE_FILE, html_index),
        ('/www/about.html', NODE_FILE, html_about),
        ('/www/scroll.html', NODE_FILE, html_scroll),
        ('/www/malformed.html', NODE_FILE, html_malformed),
        ('/www/css_test.html', NODE_FILE, html_css_test),
        ('/www/rich_page.html', NODE_FILE, html_rich_page),
        ('/www/css_malformed.html', NODE_FILE, html_css_malformed),
        ('/www/test.bmp', NODE_FILE, valid_bmp_bytes),
        ('/www/test_bad_magic.bmp', NODE_FILE, bad_magic_bmp_bytes),
        ('/www/test_oversized.bmp', NODE_FILE, oversized_bmp_bytes),
        ('/www/style_ext.css', NODE_FILE, style_ext),
        ('/www/style_invalid.css', NODE_FILE, style_invalid),
        ('/www/img_test.html', NODE_FILE, html_img_test),
        ('/www/link_css.html', NODE_FILE, html_link_css),
        ('/www/form_test.html', NODE_FILE, html_form_test),
        ('/www/form_result.html', NODE_FILE, html_form_result),
        ('/www/nav_test.html', NODE_FILE, html_nav_test),
        ('/www/script_test.js', NODE_FILE, js_external_script),
        ('/www/js_test.html', NODE_FILE, html_js_test),
        ('/www/interactive_test.html', NODE_FILE, html_interactive_test),
        ('/www/adversarial_test.html', NODE_FILE, html_adversarial_test),
    ]

    # Filter out root '/' if we only register entries below root
    # Actually keeping explicit directories:
    entries = []
    # Only entries other than "/" need to be in table, or "/" can be first
    for path, ntype, data in files:
        if path == '/':
            continue
        entries.append((path, ntype, data))

    header_size = 64
    entry_size = 144
    entry_count = len(entries)
    entries_offset = header_size
    data_offset_start = entries_offset + entry_count * entry_size

    # Align data_offset_start to 16 bytes
    if data_offset_start % 16 != 0:
        data_offset_start += 16 - (data_offset_start % 16)

    # Build entry records and data blob
    cur_data_offset = data_offset_start
    entry_records = []
    data_blobs = []

    for path, ntype, data in entries:
        if ntype == NODE_FILE:
            d_off = cur_data_offset
            d_size = len(data)
            entry_records.append((path, ntype, d_off, d_size, 0))
            data_blobs.append(data)
            cur_data_offset += d_size
            # Align next data blob to 16 bytes
            padding = (16 - (len(data) % 16)) % 16
            if padding:
                data_blobs.append(b'\x00' * padding)
                cur_data_offset += padding
        else:
            entry_records.append((path, ntype, 0, 0, 0))

    total_size = cur_data_offset

    # Pack header: <8sIIIIII32s
    hdr_bytes = struct.pack(
        '<8sIIIIII32s',
        b'ASTRAFS1',
        1,  # version
        header_size,
        total_size,
        entry_count,
        entries_offset,
        data_offset_start,
        b'\x00' * 32
    )

    # Pack entries: <128sIIII
    entries_bytes = bytearray()
    for path, ntype, d_off, d_size, flags in entry_records:
        name_bytes = path.encode('utf-8')
        if len(name_bytes) >= 128:
            raise ValueError(f"Path too long: {path}")
        entry_bytes = struct.pack('<128sIIII', name_bytes, ntype, d_off, d_size, flags)
        entries_bytes.extend(entry_bytes)

    # Pad entries to data_offset_start
    pad_len = data_offset_start - (header_size + len(entries_bytes))
    if pad_len > 0:
        entries_bytes.extend(b'\x00' * pad_len)

    archive_data = bytearray()
    archive_data.extend(hdr_bytes)
    archive_data.extend(entries_bytes)
    for blob in data_blobs:
        archive_data.extend(blob)

    with open(INITRAMFS_OUT, 'wb') as f:
        f.write(archive_data)

    print(f"[INITRAMFS] Generated archive: {INITRAMFS_OUT} ({len(archive_data)} bytes, {entry_count} entries)")
    for path, ntype, d_off, d_size, _ in entry_records:
        t_str = "DIR " if ntype == NODE_DIR else "FILE"
        print(f"  - [{t_str}] {path:30s} (offset={d_off:6d}, size={d_size:6d})")

    return INITRAMFS_OUT

if __name__ == '__main__':
    build_initramfs()
