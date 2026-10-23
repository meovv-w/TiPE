"""Observe the real Discord UI in an empty, owned profile; never replace DOM.

No login data is copied, no field is filled and no form/message is submitted.
The caller supplies the fully isolated compositor/D-Bus environment.
"""
import json
import re
import subprocess
import time
from urllib.parse import urlsplit, urlunsplit
from urllib.request import urlopen


def probe(root, real_home, private, out, env, launch, compositor_alive, samples=18):
    profile = private/'discord-profile'
    client_env = dict(env, DISCORD_USER_DATA_DIR=str(profile), WAYLAND_DEBUG='client')
    app = real_home/'.config/discord/app-1.0.162/Discord'
    assert app.is_file(), 'Installed Discord executable not found'
    process = launch([str(app), '--remote-debugging-port=0', '--ozone-platform=wayland',
                      '--enable-wayland-ime', '--wayland-text-input-version=3',
                      '--disable-features=Vulkan'], 'discord.log', client_env)
    cdp_profile = private/'discord-cdp'
    cdp_profile.mkdir()
    results = dict(scope='Unmodified Discord UI, empty isolated profile, no input or messages',
                   app=str(app), observed=[], real_ui_ready=False)
    started = time.monotonic()
    for iteration in range(samples):
        assert compositor_alive(), 'Private compositor exited'
        alive = process.poll() is None
        log = (out/'discord.log').read_text()
        port = re.search(r'DevTools listening on ws://127\.0\.0\.1:(\d+)/', log)
        observation = dict(elapsed_seconds=round(time.monotonic()-started, 3), launcher_alive=alive, pages=[])
        if port:
            (cdp_profile/'DevToolsActivePort').write_text(port[1]+'\n')
            try:
                with urlopen('http://127.0.0.1:'+port[1]+'/json/list', timeout=3) as response:
                    targets = json.load(response)
                for target in targets:
                    if target.get('type') != 'page':
                        continue
                    # Empty profiles have no login information, but omit URL
                    # queries/fragments from durable observations regardless.
                    url = target.get('url', '')
                    parsed = urlsplit(url)
                    safe_url = urlunsplit((parsed.scheme, parsed.netloc, parsed.path, '', ''))
                    expression = ('JSON.stringify({title:document.title,ready:document.readyState,'
                                  'text:document.body?.innerText?.slice(0,1200),'
                                  'fields:[...document.querySelectorAll("input,textarea,[contenteditable=true]")]'
                                  '.map(e=>({tag:e.tagName,type:e.type||"",name:e.name||"",'
                                  'placeholder:e.getAttribute("placeholder")||"",'
                                  'role:e.getAttribute("role")||""}))})')
                    output = subprocess.check_output(['node', str(root/'scripts/browser-cdp.mjs'),
                                                     str(cdp_profile), expression],
                                                    env=dict(client_env, TIPE_CDP_TARGET=url),
                                                    text=True, timeout=10)
                    page = json.loads(json.loads(output))
                    page['url'] = safe_url
                    observation['pages'].append(page)
                    if parsed.scheme == 'https' and parsed.hostname in ['discord.com', 'canary.discord.com']:
                        results['real_ui_ready'] = bool(page['fields'])
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                observation['error'] = type(error).__name__
        results['observed'].append(observation)
        (out/'discord-ui-probe.json').write_text(json.dumps(results, ensure_ascii=False, indent=2)+'\n')
        if iteration % 2 == 0:
            print('Discord real UI probe:', observation['elapsed_seconds'], 'seconds; pages:',
                  [p['url'] for p in observation['pages']], flush=True)
        if results['real_ui_ready'] or not alive:
            break
        time.sleep(5)
    subprocess.run(['grim', str(out/'discord-ui.png')], env=env, check=True, timeout=10)
    print('Discord real UI ready:', results['real_ui_ready'], flush=True)
    return results
