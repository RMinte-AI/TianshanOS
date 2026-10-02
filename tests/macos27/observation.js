// Injected only by fixture-server when ?observe is present. Never shipped.
if (new URLSearchParams(location.search).has('observe')) {
    const state = window.__macos27Observation = {
        started: Date.now(), requests: [], errors: [],
        sockets: {created: 0, closed: 0, active: 0}, samples: []
    };
    const originalFetch = window.fetch.bind(window);
    window.fetch = (...args) => {
        state.requests.push({at: Date.now(), url: String(args[0])});
        return originalFetch(...args);
    };
    addEventListener('error', e => state.errors.push({at: Date.now(), message: e.message}));
    addEventListener('unhandledrejection', e => state.errors.push({at: Date.now(), message: String(e.reason)}));
    addEventListener('DOMContentLoaded', () => {
        const output = document.createElement('pre');
        output.id = 'fixture-observation';
        output.style.cssText = 'position:fixed;bottom:0;right:0;width:280px;height:20px;overflow:auto;z-index:99999;background:white;color:black;font-size:10px';
        document.body.append(output);
        const sample = () => {
            state.samples.push({at: Date.now(), elapsed: Date.now()-state.started,
                visibility: document.visibilityState, route: location.hash,
                heap: performance.memory?.usedJSHeapSize,
                elements: document.querySelectorAll('*').length,
                resources: performance.getEntriesByType('resource').length,
                requests: state.requests.length, sockets: {...state.sockets}});
            output.textContent = JSON.stringify(state);
        };
        sample();
        // A test-observer interval, explicitly excluded from production timer claims.
        setInterval(sample, 30000);
        addEventListener('hashchange', sample);
    });
}
