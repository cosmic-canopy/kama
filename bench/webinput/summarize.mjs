// Prints a probe record (probe.mjs --out=…) as one block per step: the runner's answer, how many of the step's events the
// page had seen when the answer arrived (P3), and each event's trusted flag and fields. Usage: node summarize.mjs f.json…
import fs from 'node:fs';
for (const f of process.argv.slice(2)) {
  const r = JSON.parse(fs.readFileSync(f, 'utf8'));
  console.log(`=== ${f}: ${r.transport}${r.headless ? ' headless' : ' visible'}${r.scale ? ' scale ' + r.scale : ''}, ${r.startedToDone} ms`);
  if (!r.out) { console.log('  NO RESULT', r.stderrTail); continue; }
  console.log('  results:', JSON.stringify(r.results));
  const untrusted = r.log.filter(e => e.trusted === false).map(e => e.t);
  console.log(`  events: ${r.log.length}, untrusted: ${untrusted.length ? untrusted.join(',') : 'none'}`);
  for (const s of r.out) {
    const ev = r.log.filter(e => e.step === s.step);
    console.log(`  -- ${s.step}: runner ${s.runner.ok ? 'ok' : 'ERR ' + s.runner.err} ${s.runner.ms} ms; seen at answer ${s.eventsAtAnswer}, per frame after ${JSON.stringify(s.perFrame)}`);
    const seen = new Set();
    for (const e of ev) {
      if (e.t === 'pointermove' && seen.has('pm')) continue;
      if (e.t === 'pointermove') seen.add('pm');
      const f = [e.t];
      if (e.trusted === false) f.push('UNTRUSTED');
      if (e.pt) f.push(e.pt);
      if (e.x !== undefined && e.x !== null && /pointer|click|wheel|context/.test(e.t)) f.push(`@${e.x},${e.y}`);
      if (/key/.test(e.t)) f.push(`key=${e.key} code=${e.code} kc=${e.kc} loc=${e.loc}${e.shift ? ' shift' : ''}`);
      if (e.t === 'wheel') f.push(`dm=${e.dm} dy=${e.dy}`);
      if (e.mx) f.push(`movement=${e.mx},${e.my}`);
      if ('locked' in e) f.push(`locked=${e.locked}`);
      if (e.tgt) f.push(`on ${e.tgt}`);
      if (e.act !== undefined && e.act !== null) f.push(`act=${e.act}`);
      if (e.focus !== undefined) f.push(`hasFocus=${e.focus}`);
      if (e.active) f.push(`active=${e.active}`);
      console.log('     ' + f.join(' '));
    }
  }
}
