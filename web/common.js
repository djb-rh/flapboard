// Shared by every page: the (i) help pop-ups, and the range of each number field. Put
//   <button type="button" class="info" data-help="fields" aria-label="Help"></button>
// next to a field; hovering (or tapping, on a phone) shows the help below.
(function () {
  const fmt = '<a href="/clock#formats">change its format</a>';
  const HELP = {
    fields:
      '<b>Fields</b> fill themselves in on the sign:' +
      '<table>' +
      '<tr><td>{time}</td><td>the time (' + fmt + ')</td></tr>' +
      '<tr><td>{date}</td><td>the date (' + fmt + ')</td></tr>' +
      '<tr><td>{name} {hostname} {ip}</td><td>this sign</td></tr>' +
      '<tr><td>{place} {temp} {feels} {hi} {lo}</td><td>the weather</td></tr>' +
      '<tr><td>{cond} {hum} {wind} {units}</td><td>more weather</td></tr>' +
      '</table>' +
      'One field its own way: {time:%H:%M}, {date:%A %B %-d}.<br>' +
      '<b>|</b> starts a new row.<br>' +
      'Colour tiles: {R} {O} {Y} {G} {B} {V} {W} {K}. Argyle: {a}{b} over {c}{d}.',
    options:
      '<b>Message files</b><br>A blank line separates messages; each line is a row (| alone is an empty row).<br>' +
      'On a message\'s first line: <b>@left</b> <b>@right</b> <b>@center</b> <b>@top</b> <b>@hold 45</b> (seconds).<br>' +
      'A line starting with # is a comment.<br>Fields like {time} and {date} work in messages too.',
    formats:
      '<b>Time and date formats</b><table>' +
      '<tr><td>%-I:%M %p</td><td>3:07 PM</td></tr><tr><td>%H:%M</td><td>15:07</td></tr>' +
      '<tr><td>%a %b %-d</td><td>Tue Oct 7</td></tr><tr><td>%A %B %-d</td><td>Tuesday October 7</td></tr>' +
      '<tr><td>%-m/%-d/%y</td><td>10/7/26</td></tr></table>' +
      '%H hour 00-23, %I 01-12, %M minutes, %S seconds, %p AM/PM, %a %A day, %b %B month, %d %e day of month, %m month, %y %Y year, %j day of year. A - drops the leading zero.',
    restart:
      'The camera has to start before Wi-Fi, so turning motion sensing <b>on</b> takes a restart. ' +
      'Use the <b>Restart now</b> button that appears here (or Settings &gt; System &gt; Restart). Turning it off takes effect at once.',
  };
  function attach(b) {
    if (b.dataset.ready) return;
    b.dataset.ready = '1';
    b.textContent = 'i';
    const wrap = document.createElement('span');
    wrap.className = 'infowrap';
    b.replaceWith(wrap);
    wrap.append(b);
    const pop = document.createElement('div');
    pop.className = 'pop';
    pop.innerHTML = HELP[b.dataset.help] || b.dataset.text || '';
    wrap.append(pop);
    b.addEventListener('click', e => {
      e.preventDefault();
      const open = !pop.classList.contains('open');
      document.querySelectorAll('.pop.open').forEach(p => p.classList.remove('open'));
      if (open) pop.classList.add('open');
    });
  }
  document.addEventListener('click', e => {
    if (!e.target.closest('.info') && !e.target.closest('.pop')) document.querySelectorAll('.pop.open').forEach(p => p.classList.remove('open'));
  });
  // Every number field says what it accepts, from its own min and max.
  function range(inp) {
    if (inp.dataset.ranged || inp.min === '' || inp.max === '') return;
    inp.dataset.ranged = '1';
    const lab = inp.id && document.querySelector('label[for="' + inp.id + '"]');
    if (!lab) return;
    const r = document.createElement('span');
    r.className = 'range';
    r.textContent = ' (' + inp.min + ' to ' + inp.max + ')';
    lab.append(r);
  }
  window.fbHelp = () => {
    document.querySelectorAll('button.info').forEach(attach);
    document.querySelectorAll('input[type=number]').forEach(range);
  };
  document.addEventListener('DOMContentLoaded', window.fbHelp);
})();
