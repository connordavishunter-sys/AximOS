"""Checks Lua runs scripts and one-liners with its libraries, and that its prompt
edits lines and keeps their history."""
import time
from vm import VM, check, finish

SCRIPT = ('local t = {} for i = 1, 5 do t[i] = i * i end print(table.concat(t, ",")) '
          'local f = io.open("/tmp/x", "w") f:write("hi") f:close() print(io.open("/tmp/x"):read("a")) '
          'print(io.popen("echo piped"):read("l")) print(pcall(error, "oops")) print(os.date("%Y") >= "2026")')

v = VM()
try:
    check('prompt at boot', v.wait('/$ ', 30), v.text())
    o = v.run("lua -v; lua -e 'print(2^10, string.format(\"%5.2f\", 1/3))'", 3)
    check('lua runs a one-liner', 'Lua 5.4.7' in o and '1024.0\t 0.33' in o, o)
    o = v.run(f"echo '{SCRIPT}' > /tmp/t.lua; lua /tmp/t.lua", 3)
    check('and a script, with tables, files, pipes and errors', '1,4,9,16,25\nhi\npiped\nfalse\toops\ntrue' in o, o)

    at = v.mark()
    v.type('lua\n'); time.sleep(1)
    v.type('x = 6 * 7\n'); time.sleep(0.3)
    v.type('print(x)\n')
    check('its prompt runs what is typed', v.wait('\n42\n', 5, at), v.text(at))
    v.key('ctrl-c'); time.sleep(0.3)
    check('Ctrl+C leaves it at the prompt', v.wait('^C', 5, at), v.text(at))
    v.key('up'); v.key('ret'); time.sleep(0.5)
    check('Up brings back the line before', v.text(at).count('\n42\n') == 2, v.text(at))
    v.key('ctrl-d'); time.sleep(0.5)
    o = v.run('cat /home/.lua_history')
    check('which is kept in ~/.lua_history', 'x = 6 * 7\nprint(x)' in o, o)
finally:
    v.close()
finish()
