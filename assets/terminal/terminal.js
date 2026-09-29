'use strict';
(() => {
    const term = new Terminal({cursorBlink: true, fontFamily: 'Consolas, monospace',
        theme: {background: '#161616', foreground: '#dedede'}, scrollback: 5000});
    const fit = new FitAddon.FitAddon();
    term.loadAddon(fit);
    term.open(document.getElementById('terminal'));
    new QWebChannel(qt.webChannelTransport, channel => {
        const bridge = channel.objects.terminal;
        bridge.output.connect(encoded => {
            const raw = atob(encoded);
            const bytes = Uint8Array.from(raw, char => char.charCodeAt(0));
            // Parsing is asynchronous; return credit only when xterm consumes the bytes.
            term.write(bytes, () => bridge.ack(bytes.length));
        });
        const queued = [];
        let sending = false;
        function pump() {
            if (sending || !queued.length) return;
            sending = true;
            const bytes = queued[0];
            bridge.send(btoa(String.fromCharCode(...bytes)), accepted => {
                if (accepted) queued.shift();
                sending = false;
                if (queued.length) {
                    if (accepted) queueMicrotask(pump);
                    else setTimeout(pump, 30);
                }
            });
        }
        function submit(bytes) {
            for (let i = 0; i < bytes.length; i += 2048) queued.push(bytes.slice(i, i + 2048));
            // Don't silently drop keystrokes on a full input queue: pump retries on backpressure.
            pump();
        }
        term.onData(data => submit(new TextEncoder().encode(data)));
        // xterm's onBinary is a sequence of 8-bit code units, not UTF-8 text.
        term.onBinary(data => submit(Uint8Array.from(data, c => c.charCodeAt(0))));
        function resize() {
            if (!document.getElementById('terminal').clientWidth) return;
            fit.fit();
            if (term.cols > 0 && term.rows > 0) bridge.resize(term.cols, term.rows);
        }
        new ResizeObserver(resize).observe(document.getElementById('terminal'));
        resize();
        window.p4vpatchFocusTerminal = () => { resize(); term.focus(); };
        bridge.ready();
        // Opening the Terminal tab should show an input caret immediately.
        // P4V's View menu is already closed by the time WebChannel is ready.
        requestAnimationFrame(() => { resize(); term.focus(); });
    });
})();
