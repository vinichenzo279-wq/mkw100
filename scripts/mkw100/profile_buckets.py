#!/usr/bin/env python3
"""Splits the game thread's samples of an MKW100_TEST_PROFILE profile into cost buckets.
usage: profile_buckets.py PROFILE EXE [--nm llvm-nm] [--map MAP.txt] [--thread game]
Each sample goes to the first bucket (in the order below) with a matching frame anywhere in its stack."""
import argparse, collections, os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
import profile_report as pr

BUCKETS = [
    ('wait: GX worker sync',        r'GxWorker4Sync|GxWorker.*WaitForTail'),
    ('render: particles/effects',   r'ScnProcEffects::Draw|Effects::Mgr::Draw|EffectManager::Draw'),
    ('render: HUD/2D',              r'Section::Draw|RendererRaceUIOrtho|RendererRaceUI'),
    ('render: models (draw)',       r'GameScene::draw|ScnMgr::DrawMain'),
    ('sim: CPU AI',                 r'^AI::|KartAIController'),
    ('sim: items',                  r'^Item::|ItemDirector|Item::Manager'),
    ('sim: kart physics/collision', r'Kart::Manager::Update|Kart::Player|^KCL|CourseMgr::IsColliding'),
    ('render: model calc (anim, matrices)', r'ScnMgr::CalcAll|ScnRoot::CalcAnm|ScnMgr::UpdateScnRoot|ModelDirector::Update'),
    ('render: effects calc',        r'Effects::Mgr::Update|ef::.*Calc|EffectManager::Calc'),
    ('audio',                       r'^Audio::|nw4r::snd|Audio_HLE|AxDspHle'),
    ('sim: objects/course/other',   r'GameScene::calc|RaceScene::OnCalc'),
    ('frame pacing / waits (VI, OS)', r'VI_HLE|SleepPrecisely|OS::SleepThread|SelectThread|poll_events'),
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile'); ap.add_argument('exe')
    ap.add_argument('--nm', default='llvm-nm')
    ap.add_argument('--map', default=os.path.join(os.path.dirname(__file__), '../../projects/mkwii/MAP.txt'))
    ap.add_argument('--thread', default='game')
    a = ap.parse_args()
    addrs, names = pr.load_nm(a.exe, a.nm)
    gmap = pr.load_map(a.map)
    import bisect
    gkeys = sorted(gmap)
    mods, stacks, base = [], [], None
    for line in open(a.profile):
        p = line.split()
        if not p: continue
        if p[0] == 'module':
            b = int(p[1], 16); mods.append((b, b + int(p[2], 16), os.path.basename(p[3])))
            if base is None: base = b
        elif p[0] == 's' and p[1] == a.thread:
            stacks.append([int(x, 16) for x in p[2:]])
    mods.sort(); ms = [m[0] for m in mods]; cache = {}
    def res(pc):
        r = cache.get(pc)
        if r is None:
            i = bisect.bisect_right(ms, pc) - 1
            if i < 0 or pc >= mods[i][1]: r = '?'
            elif mods[i][0] == base:
                j = bisect.bisect_right(addrs, pc - base + 0x140000000) - 1
                r = names[j] if j >= 0 else '?'
                g = pr.GUEST.search(r)
                if g: r = gmap.get(int(g.group(1), 16), r)
            else: r = mods[i][2]
            cache[pc] = r
        return r
    rx = [(n, re.compile(p)) for n, p in BUCKETS]
    c = collections.Counter()
    for s in stacks:
        rs = [res(pc) for pc in s]
        for n, r in rx:
            if any(r.search(x) for x in rs):
                c[n] += 1; break
        else:
            c['other'] += 1
    n = len(stacks)
    print('%d samples (%s thread)' % (n, a.thread))
    for name, _ in BUCKETS + [('other', None)]:
        print('  %5.1f%%  %s' % (100.0 * c[name] / n if n else 0, name))

if __name__ == '__main__':
    main()
