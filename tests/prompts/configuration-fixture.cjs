// HTTP fixture for the production page scripts. Storage/reference semantics are
// covered by the native owner tests; this fixture checks what the page sends.
const version = n => n.toString(16).padStart(32, '0');
const named = 'Quoted “模板” <&> ';
function fixture() {
    const state = {revision: 1, source: 'sd', paused: false, sd_available: true, backup: false,
        previewError: false, forceNeedsPause: false, deleted: false, calls: []};
    const status = (extra = {}) => ({active_version: version(state.revision), source: state.source,
        loaded: true, configured: true, enabled: !state.paused, paused: state.paused, uncertain: false,
        sd_available: state.sd_available, awaiting_sd_choice: state.source === 'internal' && state.sd_available,
        internal_exists: state.backup, internal_valid: state.backup, comparison_known: state.backup,
        backup_matches: false, internal_saved_at: 1700000000, ...extra});
    const action = {id: 'act', name: named, type: 'cli', enabled: true, cli: {command: 'help', timeout_ms: 5000}};
    const actions = () => state.deleted ? [] : [action, {...action, id: 'replacement', name: 'Replacement'}];
    const impact = () => ({rules: [{id: 'r1', name: 'Rule <one>'}], actions: [], commands: [], sources: [],
        references_confirmed: !state.forceNeedsPause, requires_pause: state.forceNeedsPause});
    const ok = data => ({code: 0, message: 'ok', data});
    const fail = (reason, extra) => ({code: 4, message: reason, data: status(extra)});
    const respond = call => {
        state.calls.push(call);
        const {endpoint: e, params: p} = call;
        if (e === 'automation/config/status' || e === 'automation/config/refresh') return ok(status());
        if (e === 'automation/config/backup') {state.backup = true;return ok(status({saved: true}));}
        if (e === 'automation/config/preview') return ok({...status(), operation: p.operation, target_version: 'target-one',
            will_remain_paused: state.paused, diff: {added: [{collection: 'rules', id: 'sd-rule', name: 'Card rule'}], changed: [], removed: [], complete: true}});
        if (e === 'automation/config/apply') {
            if (p.active_version !== version(state.revision) || p.target_version !== 'target-one') return fail('configuration_changed');
            if (p.operation !== 'restore_internal') state.source = 'sd';
            ++state.revision;return ok(status({saved: p.operation !== 'use_sd'}));
        }
        if (e === 'automation/config/enable') {state.paused = !p.enabled;++state.revision;return ok(status({saved: true}));}
        if (e === 'automation/rules/list') return ok({...status(), rules: [{id: 'r1', name: 'Rule <one>', enabled: false, revision: 1, actions_count: 0, actions: [], conditions: []}]});
        if (e === 'automation/actions/list') return ok({...status(), templates: actions()});
        if (e === 'automation/actions/get') return ok({...action, active_version: version(state.revision)});
        if (e === 'automation/sources/list') return ok({...status(), sources: []});
        if (e === 'ssh/commands/list') return ok({...status(), commands: []});
        if (e === 'ssh/hosts/list') return ok({...status(), hosts: []});
        if (e === 'automation/status') return ok({state: 'running', rule_count: 1, variable_count: 0, uptime: 0});
        if (e === 'automation/actions/add' || e === 'automation/actions/update') {
            if (p.active_version !== version(state.revision)) return fail('configuration_changed');
            ++state.revision;return ok(status({saved: true}));
        }
        if (e === 'automation/actions/delete') {
            if (p.active_version !== version(state.revision)) return fail('configuration_changed');
            if (p.preview) return state.previewError ? fail('configuration_invalid') : ok({...status(), impact: impact()});
            if (p.force && state.forceNeedsPause && !p.pause_automation) return fail('pause_required', {impact: impact()});
            if (p.pause_automation) state.paused = true;
            state.deleted = true;++state.revision;return ok(status({saved: true, impact: impact()}));
        }
        if (e.endsWith('/import') && /^(automation\/(rules|sources|actions)|ssh\/(commands|hosts))\/import$/.test(e)) {
            if (p.active_version !== version(state.revision)) return fail('configuration_changed');
            if (p.preview) return ok({...status(), valid: true, trusted: true, target_matches: true, id: 'imported', exists: true,
                signer: 'Test signer', package_digest: 'a'.repeat(64), credential_generation: 7, expected_revision: 2, expected_generation: 3,
                items: [{collection: e.includes('commands') ? 'hosts' : 'actions', id: 'dependency'}],
                ...(e.includes('rules') ? {rule: {id: 'imported', name: 'Imported rule', conditions: [], actions: []}} : {})});
            ++state.revision;
            return ok(e.includes('rules') ? {active_version: version(state.revision), saved: true, durable: true, runtime_applied: true, imported: true} : status({saved: true, imported: true}));
        }
        return ok({});
    };
    return {state, respond, status};
}
module.exports = {fixture, version, named};
