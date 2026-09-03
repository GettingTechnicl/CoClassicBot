import frida, sys, time

# Live test: replay the EXACT packet sequence captured from a manual
# meteor -> scroll craft at MillionaireLee (ActivateNpc, then AnswerNpc(0)
# twice, ~1s apart) by calling coclassic.dll's own already-proven
# CHero::ActivateNpc / CHero::AnswerNpcEx directly via Frida, on the
# ALREADY-RUNNING process -- no rebuild/relaunch needed. Addresses resolved
# from the currently-built coclassic.pdb via tools/resolve_symbol_addrs.ps1:
#   CHero::GetSingletonPtr  RVA=0x3BDD0
#   CHero::ActivateNpc      RVA=0x3A6F0  (this, OBJID idNpc)
#   CHero::AnswerNpcEx      RVA=0x3A820  (this, int answer, int taskId)
#
# usage: python frida_millionaire_test.py <pid> <npcId>

pid = int(sys.argv[1])
npc_id = int(sys.argv[2])

session = frida.get_local_device().attach(pid)
script = session.create_script(r'''
const base = Process.getModuleByName('coclassic.dll').base;
const getSingletonPtr = new NativeFunction(base.add(0x3BDD0), 'pointer', [], 'win64');
const activateNpcFn   = new NativeFunction(base.add(0x3A6F0), 'void', ['pointer', 'uint32'], 'win64');
const answerNpcExFn   = new NativeFunction(base.add(0x3A820), 'void', ['pointer', 'int32', 'int32'], 'win64');

function hero() { return getSingletonPtr(); }

rpc.exports = {
    heroPtr() {
        const h = hero();
        return h.isNull() ? null : h.toString();
    },
    activateNpc(npcId) {
        const h = hero();
        if (h.isNull()) return { error: 'no hero' };
        activateNpcFn(h, npcId);
        return { ok: true, hero: h.toString() };
    },
    answerNpc(answer, taskId) {
        const h = hero();
        if (h.isNull()) return { error: 'no hero' };
        answerNpcExFn(h, answer, taskId);
        return { ok: true, hero: h.toString() };
    }
};
''')
script.on('message', lambda msg, data: print('[frida msg]', msg))
script.load()

print(f'Attached to PID {pid}. Hero: {script.exports_sync.hero_ptr()}')

print(f'Step 1: ActivateNpc({npc_id})')
print(' ->', script.exports_sync.activate_npc(npc_id))
time.sleep(1.2)

print('Step 2: AnswerNpc(0) [taskId=101]')
print(' ->', script.exports_sync.answer_npc(0, 101))
time.sleep(1.2)

print('Step 3: AnswerNpc(0) again [matching the captured double-send]')
print(' ->', script.exports_sync.answer_npc(0, 101))

print('Done. Check your bag: Meteor count should drop by 10, MeteorScroll count +1.')
session.detach()
