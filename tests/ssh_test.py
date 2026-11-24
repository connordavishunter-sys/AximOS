"""Checks OpenSSH: sshd is off unless turned on, logs in with a key made by ssh-keygen,
running commands for ssh, and git clones and pushes over it, as to GitHub."""
from vm import VM, check, finish

v = VM()
try:
    v.login()
    out = v.run('ps')
    check("sshd is off, as it hasn't been turned on", 'sshd' not in out, out)
    out = v.sudo('ssh-keygen -A >/dev/null && /system/bin/sshd && echo started', 4)
    check('it starts, making its host keys', 'started' in out, out)

    v.run("ssh-keygen -q -t ed25519 -N '' -f ~/.ssh/id_ed25519 && cp ~/.ssh/id_ed25519.pub ~/.ssh/authorized_keys", 3)
    at = v.mark()
    v.type('ssh -o StrictHostKeyChecking=accept-new localhost "echo hello from \\$(hostname) over ssh"\n')
    check('ssh logs in with a key, and runs a command', v.wait('hello from aximos over ssh', 20, at), v.text(at))

    v.run("git config --global user.name Tester; git config --global user.email tester@aximos")
    v.run('git init -q --bare -b main remote.git', 1)
    out = v.run('git clone -q localhost:remote.git work 2>&1; cd work && echo hi > a && git add a && git commit -qm First && '
                'git push -q origin main 2>&1; cd; git -C remote.git log --oneline | cat', 10)
    check('git clones and pushes over ssh', 'First' in out, out)
finally:
    v.close()
finish()
