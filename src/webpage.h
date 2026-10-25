#pragma once

// Settings page, placeholders are %NAME% and get replaced in main.cpp.
static const char SETTINGS_PAGE[] = R"html(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OpenBird</title>
<style>
body{font-family:system-ui,sans-serif;max-width:32rem;margin:0 auto;padding:1rem;color:#222;background:#fafafa}
h1{font-size:1.4rem}h2{font-size:1.1rem;margin-top:1.5rem}
table{border-collapse:collapse;width:100%}td{padding:.25rem .5rem .25rem 0;vertical-align:top}td:first-child{color:#666;width:40%}
label{display:block;margin-top:.75rem;color:#444}input{width:100%;box-sizing:border-box;padding:.4rem;font-size:1rem}
button{margin-top:1rem;padding:.5rem 1rem;font-size:1rem}.msg{padding:.5rem;background:#e6f4ea;border-radius:4px}
.err{background:#fce8e6}small{color:#666}
</style></head><body>
<h1>OpenBird</h1>
%MESSAGE%
<table>
<tr><td>Device</td><td>%DEVICE%</td></tr>
<tr><td>Firmware</td><td>%FIRMWARE%</td></tr>
<tr><td>MAC</td><td>%MAC%</td></tr>
<tr><td>IP</td><td>%IP%</td></tr>
<tr><td>Host</td><td>%HOST%.local</td></tr>
<tr><td>Uptime</td><td>%UPTIME%</td></tr>
<tr><td>Doorbell</td><td>%DOORBELL%</td></tr>
<tr><td>Event clients</td><td>%MONITORS%</td></tr>
<tr><td>Notifications</td><td>%NOTIFY%</td></tr>
<tr><td>Time</td><td>%TIME%</td></tr>
</table>

<h2>Relay test</h2>
<button type="button" onclick="fetch('/bha-api/open-door.cgi?r=1')">Pulse relay 1</button>

<h2>Settings</h2>
<form method="post" action="/settings">
<input type="hidden" name="token" value="%TOKEN%">
<label>Username<input name="user" value="%USER%" maxlength="32" required></label>
<label>New password <small>(leave empty to keep)</small><input name="password" type="password" maxlength="64" autocomplete="new-password"></label>
<label>Relay pulse (ms)<input name="pulse" type="number" min="100" max="30000" value="%PULSE%" required></label>
<button type="submit">Save</button>
<p><small>Changing the username or password disconnects all clients. Re-pair the device in nymea with the new credentials.</small></p>
</form>
</body></html>
)html";
