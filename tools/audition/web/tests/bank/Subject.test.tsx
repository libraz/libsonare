import { render, screen } from '@testing-library/react';
import { beforeEach, describe, expect, it } from 'vitest';
import { resetBank } from '../../src/bank/store';
import { RESOURCES, setLang } from '../../src/i18n';
import { setState } from '../../src/state/store';
import { HeadStage } from '../../src/voice/HeadStage';
import { Subject } from '../../src/voice/Subject';
import { bank, manifest, setEntry, voice } from './fixtures';

const en = RESOURCES.en;
const subject = () => document.getElementById('subject') as HTMLElement;
const cell = (cls: string) => subject().querySelector(`.${cls}`) as HTMLElement;
const cells = () => [...subject().querySelectorAll('.idcell')] as HTMLElement[];

const modelOnly = () =>
  manifest({
    provenance: { want: { branch: 'default', timbre: 'instrument' }, state: 'uncaptured' },
  });

const withReference = () =>
  manifest({
    provenance: {
      want: { branch: 'machine_defined', timbre: 'machine', reason: 'the machine invented it' },
      state: 'aimed',
      capture: { source_class: 'module', product: 'Cardboard Rack', room: 'none' },
      approximated: { answered_by: '12', reason: 'stand-in' },
      answers: ['3', '4'],
    },
  });

beforeEach(() => {
  resetBank({ voices: bank(), loaded: true });
  setState({ setId: 'p000', sets: [setEntry('p000')] });
});

describe('Subject', () => {
  it('renders nothing before a manifest names a voice', () => {
    render(<Subject />);
    expect(subject()).toBeEmptyDOMElement();
  });

  it('puts the voice on the map, counting from one and from zero, with the method and patch', () => {
    setState({ manifest: modelOnly() });
    render(<Subject />);
    expect(cells()).toHaveLength(3);
    const c = cells()[0] as HTMLElement;
    expect(c.querySelector('.idhead')?.textContent).toBe(en['id.voice']);
    expect(c.querySelector('.idname')?.textContent).toBe(
      'Glass HarmonicaGM 1 · program 0 · bank 0',
    );
    expect(c.querySelector('.idtag[data-slot="gm"]')).not.toBeNull();
    expect(c.querySelector('.idtag[data-method="physical"]')).not.toBeNull();
    expect(c.textContent).toContain('modal · patch glass');
  });

  it('reads the engine from the bank row, and says so when the bank has none', () => {
    setState({ manifest: modelOnly(), setId: 'nowhere' });
    render(<Subject />);
    expect(cells()[0]?.textContent).toContain(`${en['bank.notReported']} · patch glass`);
    expect(cells()[0]?.querySelector('.idtag[data-method="classic"]')).not.toBeNull();
  });

  it('says when a patch is not reported', () => {
    setState({
      manifest: manifest({ voice: { name: 'Glass Harmonica', program: 0, bank: 0 } }),
    });
    render(<Subject />);
    expect(cells()[0]?.textContent).toContain(en['subj.noPatch']);
  });

  it('words the aim for a model-only page and says nothing is captured', () => {
    setState({ manifest: modelOnly(), setId: 'nowhere' });
    render(<Subject />);
    const target = cell('id-target');
    expect(target).toHaveClass('role-reference');
    expect(target.querySelector('.idbig')?.textContent).toBe(en['tgt.default']);
    expect(target.querySelector('.idstate')?.textContent).toBe(en['tst.uncaptured']);
    expect(target.querySelector('.idlong')).toBeNull();
  });

  it('says there is no policy for a slot outside the repository', () => {
    setState({ manifest: manifest(), setId: 'nowhere' });
    render(<Subject />);
    expect(cell('id-target').textContent).toContain(en['id.noPolicy']);
  });

  it('names the capture, its kind and its room, with the policy reason on hover', () => {
    setState({ manifest: withReference(), setId: 'nowhere' });
    render(<Subject />);
    const target = cell('id-target');
    expect(target.querySelector('.idbig')?.textContent).toBe(en['tgt.machine_defined']);
    expect(target.querySelector('.idbig')).toHaveAttribute('title', 'the machine invented it');
    expect(target.querySelector('.idstate')?.textContent).toBe(en['tst.aimed']);
    expect(target.querySelector('.idlong')?.textContent).toBe(
      `${en['src.module']} · Cardboard Rack · ${en['room.none']}`,
    );
    expect(target.querySelector('.idline.warn')?.textContent).toBe('approximated by 12');
    expect(target.querySelector('.idline.warn')).toHaveAttribute('title', 'stand-in');
    expect(target.querySelector('.idline.dim')?.textContent).toBe('also stands in for 3, 4');
  });

  it('reads the room from the measurement first, then from the effects switch', () => {
    const room = (cap: Record<string, unknown>) => {
      setState({
        manifest: manifest({ provenance: { want: { branch: 'default' }, capture: cap } }),
        setId: 'nowhere',
      });
      const { unmount } = render(<Subject />);
      const text = cell('id-target').querySelector('.idlong')?.textContent ?? '';
      unmount();
      return text;
    };
    expect(room({ room: { rt60: 0.4 } })).toContain(en['room.present']);
    expect(room({ dry: true })).toContain(en['room.dry']);
    expect(room({ dry: false })).toContain(en['room.undeclared']);
    expect(room({ room: 'none', dry: false })).toContain(en['room.none']);
  });

  it('withholds the capture and the stand-ins when blind', () => {
    setState({ manifest: withReference(), blind: true, setId: 'nowhere' });
    render(<Subject />);
    const target = cell('id-target');
    expect(target.textContent).toContain(en['id.blindHidden']);
    expect(target.textContent).not.toContain('Cardboard Rack');
    expect(target.querySelector('.idstate')).toBeNull();
    expect(target.querySelector('.idline')).toBeNull();
  });

  it('does not name another comparison’s reference for one that has no oracle', () => {
    setState({
      manifest: manifest({
        provenance: { want: { branch: 'default' }, capture: { product: 'Cardboard Rack' } },
        comparisons: [{ id: 'solo', status: 'matched', model_sources: ['a'], oracle_sources: [] }],
      }),
      setId: 'nowhere',
    });
    render(<Subject />);
    const target = cell('id-target');
    expect(target.textContent).not.toContain('Cardboard Rack');
    expect(target.querySelector('.idlong')?.textContent).toBe(en['compare.noReference']);
  });

  it('says why a slot is declined', () => {
    setState({
      manifest: manifest({
        provenance: {
          want: { branch: 'variations' },
          declined: { names: 'Tone A', carries: 'Tone B', reason: 'maps differ' },
        },
      }),
      setId: 'nowhere',
    });
    render(<Subject />);
    const line = cell('id-target').querySelector('.idlong');
    expect(line?.textContent).toBe(
      'the map names this slot Tone A; the recordings hold Tone B here',
    );
    expect(line).toHaveAttribute('title', 'maps differ');
  });

  it('names the bank row’s target ahead of the page’s policy', () => {
    resetBank({
      voices: [voice({ slug: 'p000', target: { branch: 'kits', timbre: 'machine' } })],
      loaded: true,
    });
    setState({ manifest: modelOnly() });
    render(<Subject />);
    expect(cell('id-target').querySelector('.idbig')?.textContent).toBe(en['tgt.kits']);
  });

  it('names the instruments a kit take strikes, in the order struck, once each', () => {
    setState({
      setId: 'k000',
      manifest: manifest({
        voice: { name: 'Cardboard Kit', program: 0, kit: true },
        provenance: { drum_names: { '36': 'Boom' } },
      }),
      items: [
        {
          id: 't',
          tracks: {},
          meta: {
            notes: [
              { start: 0.5, note: 38, velocity: 90 },
              { start: 0.0, note: 36, velocity: 90 },
              { start: 1.0, note: 36, velocity: 90 },
            ],
          },
        },
      ],
      itemIndex: 0,
    });
    render(<Subject />);
    const c = cells()[0] as HTMLElement;
    expect(c.querySelector('.idname')?.textContent).toBe('Cardboard Kitkit 0 · channel 10');
    expect(c.querySelector('.idtag[data-slot="kit"]')).not.toBeNull();
    expect(c.textContent).toContain(`${en['subj.struck']}Boom 36 · note 38`);
  });

  it('leaves the struck line out of a melodic voice', () => {
    setState({ manifest: modelOnly() });
    render(<Subject />);
    expect(subject().textContent).not.toContain(en['subj.struck']);
  });
});

describe('Subject ship cell', () => {
  const shipsOf = (over: Record<string, unknown>) =>
    setState({
      manifest: modelOnly(),
      sets: [setEntry('p000', { ships: over })],
    });

  it('says a current page is what ships', () => {
    shipsOf({ state: 'current', rendered: '2026-10-02' });
    render(<Subject />);
    const ships = cells()[2] as HTMLElement;
    expect(ships.querySelector('.idstate')).toHaveAttribute('data-state', 'current');
    expect(ships.textContent).toContain(en['ship.current']);
    expect(ships.textContent).toContain('rendered 2026-10-02');
    expect(ships.querySelector('code')).toBeNull();
  });

  it('flags a stale page, counts the changes since, and prints the command that renders it again', () => {
    shipsOf({
      state: 'stale',
      rendered: '2026-09-01',
      // biome-ignore lint/suspicious/noThenProperty: the field is named by the server
      then: { glass: 2 },
      since: [{ unit: 'glass', version: 3, date: '2026-10-01', note: 'damping' }],
      reasons: ['glass moved'],
    });
    resetBank({
      voices: [voice({ slug: 'p000', ships: { unit: 'glass', version: 3, date: '2026-10-01' } })],
      loaded: true,
    });
    setState({ manifest: manifest({ voice: { name: 'Glass Harmonica', program: 40, bank: 8 } }) });
    render(<Subject />);
    const ships = cells()[2] as HTMLElement;
    expect(ships.querySelector('.idstate')).toHaveAttribute('data-state', 'stale');
    expect(ships.textContent).toContain('ships: glass v3 (2026-10-01)');
    expect(ships.textContent).toContain('this page: glass v2, rendered 2026-09-01');
    const since = ships.querySelector('.idsince') as HTMLElement;
    expect(since.textContent).toBe('1 changes since');
    expect(since.title).toBe('glass moved\nglass v3  2026-10-01  damping');
    const cmd = ships.querySelector('code') as HTMLElement;
    expect(cmd.textContent).toBe('tools/voicematch/make_audition.py --program 40 --banks 8');
    expect(cmd).toHaveAttribute('title', en['ship.rerenderTitle']);
  });

  it('says a kit page holds several versioned units', () => {
    resetBank({ voices: [], loaded: true });
    setState({
      manifest: modelOnly(),
      sets: [setEntry('p000', { ships: { state: 'current', units: [1, 2, 3] } })],
    });
    render(<Subject />);
    expect(cells()[2]?.textContent).toContain('3 drum notes');
  });

  it('has the no-record chip for a page with no ledger entry', () => {
    setState({ manifest: modelOnly(), sets: [] });
    render(<Subject />);
    expect(cells()[2]?.querySelector('.idstate')).toHaveAttribute('data-state', 'none');
  });
});

describe('Subject in both languages', () => {
  it('follows the language and never shows a key', async () => {
    setState({ manifest: withReference(), setId: 'nowhere' });
    render(<Subject />);
    setLang('ja');
    await screen.findByText(RESOURCES.ja['id.voice']);
    expect(cells().map((c) => c.querySelector('.idhead')?.textContent)).toEqual([
      RESOURCES.ja['id.voice'],
      RESOURCES.ja['id.target'],
      RESOURCES.ja['id.ships'],
    ]);
    expect(subject().textContent).not.toMatch(
      /\b(id|subj|tgt|tst|ship|src|room|slot|method)\.[a-zA-Z]+/,
    );
    expect(cell('id-target').querySelector('.idbig')?.textContent).toBe(
      RESOURCES.ja['tgt.machine_defined'],
    );
  });
});

describe('HeadStage', () => {
  it('is empty for a set the bank does not know', () => {
    setState({ setId: 'nowhere' });
    render(<HeadStage />);
    expect(document.getElementById('headStage')).toBeEmptyDOMElement();
  });

  it('carries method, rung and the unwritten settings, with the next action on hover', () => {
    setState({ setId: 'p002' });
    render(<HeadStage />);
    const head = document.getElementById('headStage') as HTMLElement;
    expect(head.querySelector('.dot')).toHaveClass('fm');
    expect(head.textContent).toContain('fm');
    expect(head.textContent).toContain(`1.0 ${en['stage.5']}`);
    expect(head.querySelectorAll('.bar .seg.on')).toHaveLength(5);
    expect(head.querySelector('.badge')?.textContent).toBe('2 unwritten');
    expect(head.querySelector('.badge')).toHaveAttribute('title', 'rubber.tension, rubber.damp');
    expect(head).toHaveAttribute('title', 'record a listening verdict');
  });

  it('omits the badge when nothing is unwritten, and names a missing engine', () => {
    setState({ setId: 'p003' });
    render(<HeadStage />);
    const head = document.getElementById('headStage') as HTMLElement;
    expect(head.querySelector('.badge')).toBeNull();
    expect(head.textContent).toContain(en['bank.notReported']);
    expect(head.textContent).toContain(`0.8 ${en['stage.4']}`);
  });

  it('words the rung in the language in force', async () => {
    setState({ setId: 'p002' });
    render(<HeadStage />);
    setLang('ja');
    await screen.findByText(new RegExp(RESOURCES.ja['stage.5']));
  });
});
