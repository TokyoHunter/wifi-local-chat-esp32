#pragma once

const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<meta name="theme-color" content="#111417">
<title>ESP32 Chat</title>
<style>
  :root{
    --bg:#0f1115; --panel:#171a21; --panel2:#1e222b; --border:#2a2f3a;
    --text:#e6e8eb; --muted:#8a90a0; --accent:#4f8cff; --danger:#ff5c5c; --ok:#3ecf8e;
  }
  *{box-sizing:border-box;}
  html,body{height:100%;}
  body{
    margin:0; background:var(--bg); color:var(--text);
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;
    display:flex; flex-direction:column; height:100vh; overflow:hidden;
  }
  header{
    display:flex; align-items:center; justify-content:space-between;
    padding:10px 14px; background:var(--panel); border-bottom:1px solid var(--border);
    flex-shrink:0;
  }
  header h1{font-size:15px; margin:0; font-weight:600;}
  .status{display:flex; align-items:center; gap:6px; font-size:12px; color:var(--muted);}
  .dot{width:8px; height:8px; border-radius:50%; background:var(--danger);}
  .dot.on{background:var(--ok);}
  .dot.mid{background:#e0b84c;}
  #userCount{font-size:12px; color:var(--muted); margin-left:10px;}

  #chat{flex:1; overflow-y:auto; padding:10px 12px; display:flex; flex-direction:column; gap:6px;}
  .msg{max-width:82%; padding:7px 10px; border-radius:10px; background:var(--panel2); word-wrap:break-word; align-self:flex-start;}
  .msg.me{align-self:flex-end; background:var(--accent); color:#fff;}
  .msg .meta{display:flex; gap:6px; font-size:11px; color:var(--muted); margin-bottom:2px;}
  .msg.me .meta{color:rgba(255,255,255,0.75);}
  .msg .body{font-size:14px; white-space:pre-wrap;}
  .sys{align-self:center; font-size:12px; color:var(--muted); padding:2px 10px;}
  .err{align-self:center; font-size:12px; color:var(--danger); padding:2px 10px;}

  form#composer{
    display:flex; gap:8px; padding:10px; background:var(--panel);
    border-top:1px solid var(--border); flex-shrink:0;
  }
  #msgInput{
    flex:1; background:var(--panel2); color:var(--text); border:1px solid var(--border);
    border-radius:20px; padding:10px 14px; font-size:14px; outline:none;
  }
  #sendBtn{
    background:var(--accent); color:#fff; border:none; border-radius:20px;
    padding:0 18px; font-size:14px; font-weight:600; cursor:pointer;
  }
  #sendBtn:disabled{opacity:0.5;}

  #joinOverlay{
    position:fixed; inset:0; background:rgba(0,0,0,0.6);
    display:flex; align-items:center; justify-content:center; padding:20px; z-index:10;
  }
  #joinBox{
    background:var(--panel); border:1px solid var(--border); border-radius:14px;
    padding:22px; width:100%; max-width:320px; text-align:center;
  }
  #joinBox h2{margin:0 0 6px; font-size:17px;}
  #joinBox p{margin:0 0 16px; font-size:13px; color:var(--muted);}
  #nameInput{
    width:100%; padding:10px 12px; border-radius:10px; border:1px solid var(--border);
    background:var(--panel2); color:var(--text); font-size:14px; margin-bottom:12px; outline:none;
  }
  #joinBtn{
    width:100%; padding:10px; border:none; border-radius:10px; background:var(--accent);
    color:#fff; font-size:14px; font-weight:600; cursor:pointer;
  }
  #joinErr{color:var(--danger); font-size:12px; min-height:16px; margin-top:8px;}
  .hidden{display:none !important;}
</style>
</head>
<body>

<div id="joinOverlay">
  <div id="joinBox">
    <h2>Join Chat Room</h2>
    <p>Pick a username to start chatting (local, no internet).</p>
    <input id="nameInput" maxlength="20" placeholder="Username" autocomplete="off">
    <button id="joinBtn">Join</button>
    <div id="joinErr"></div>
  </div>
</div>

<header>
  <h1>ESP32 Chat</h1>
  <div style="display:flex;align-items:center;">
    <div class="status"><span class="dot" id="statusDot"></span><span id="statusText">Connecting</span></div>
    <span id="userCount">&bull; 0 online</span>
  </div>
</header>

<div id="chat"></div>

<form id="composer">
  <input id="msgInput" maxlength="200" placeholder="Type a message..." autocomplete="off" disabled>
  <button id="sendBtn" type="submit" disabled>Send</button>
</form>

<script>
(function(){
  var chat = document.getElementById('chat');
  var msgInput = document.getElementById('msgInput');
  var sendBtn = document.getElementById('sendBtn');
  var composer = document.getElementById('composer');
  var statusDot = document.getElementById('statusDot');
  var statusText = document.getElementById('statusText');
  var userCount = document.getElementById('userCount');
  var joinOverlay = document.getElementById('joinOverlay');
  var nameInput = document.getElementById('nameInput');
  var joinBtn = document.getElementById('joinBtn');
  var joinErr = document.getElementById('joinErr');

  var username = '';
  var joined = false;
  var ws = null;
  var reconnectTimer = null;

  function setStatus(state, text){
    statusDot.className = 'dot ' + state;
    statusText.textContent = text;
  }

  function scrollToBottom(){
    chat.scrollTop = chat.scrollHeight;
  }

  function addBubble(user, text, time){
    var wrap = document.createElement('div');
    wrap.className = 'msg' + (user === username ? ' me' : '');
    var meta = document.createElement('div');
    meta.className = 'meta';
    meta.textContent = user + ' \u00B7 ' + time;
    var body = document.createElement('div');
    body.className = 'body';
    body.textContent = text; // textContent = safe, no HTML injection
    wrap.appendChild(meta);
    wrap.appendChild(body);
    chat.appendChild(wrap);
    scrollToBottom();
  }

  function addSystem(text, isError){
    var el = document.createElement('div');
    el.className = isError ? 'err' : 'sys';
    el.textContent = text;
    chat.appendChild(el);
    scrollToBottom();
  }

  function connect(){
    setStatus('mid', 'Connecting');
    ws = new WebSocket('ws://' + location.hostname + '/ws');

    ws.onopen = function(){
      setStatus('on', 'Connected');
      if (username){
        ws.send('JOIN:' + username);
      }
    };

    ws.onclose = function(){
      setStatus('', 'Disconnected');
      joined = false;
      msgInput.disabled = true;
      sendBtn.disabled = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
      reconnectTimer = setTimeout(connect, 2000);
    };

    ws.onerror = function(){
      ws.close();
    };

    ws.onmessage = function(evt){
      var d = evt.data;
      var type = d.charAt(0);
      var rest = d.slice(2);

      if (type === 'M'){
        var parts = rest.split('|');
        if (parts.length >= 3){
          addBubble(parts[0], parts[1], parts[2]);
        }
      } else if (type === 'S'){
        addSystem(rest, false);
      } else if (type === 'U'){
        userCount.textContent = '\u2022 ' + rest + ' online';
      } else if (type === 'E'){
        if (!joined){
          joinErr.textContent = rest;
        } else {
          addSystem(rest, true);
        }
      } else if (type === 'J'){
        joined = true;
        joinOverlay.classList.add('hidden');
        msgInput.disabled = false;
        sendBtn.disabled = false;
        msgInput.focus();
      }
    };
  }

  joinBtn.addEventListener('click', function(){
    var name = nameInput.value.trim();
    if (!name){
      joinErr.textContent = 'Please enter a username';
      return;
    }
    if (name.length > 20) name = name.substring(0, 20);
    username = name;
    joinErr.textContent = '';
    connect();
  });

  nameInput.addEventListener('keydown', function(e){
    if (e.key === 'Enter'){ e.preventDefault(); joinBtn.click(); }
  });

  composer.addEventListener('submit', function(e){
    e.preventDefault();
    var text = msgInput.value.trim();
    if (!text || !ws || ws.readyState !== 1) return;
    if (text.length > 200) text = text.substring(0, 200);
    ws.send('MSG:' + text);
    msgInput.value = '';
  });
})();
</script>
</body>
</html>
)HTMLPAGE";