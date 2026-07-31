// The home page's only JavaScript. Everything here is an enhancement: with JS disabled the OS
// tabs still switch (they are radio buttons), all three install commands are reachable, and the
// copy buttons stay hidden rather than sitting there dead. Docs pages ship no script at all.
(() => {
  const platform = (navigator.userAgentData?.platform || navigator.platform || '').toLowerCase();
  const os = platform.includes('win') ? 'windows'
           : platform.includes('linux') || platform.includes('x11') ? 'linux'
           : 'mac';
  const radio = document.getElementById('os-' + os);
  if (radio) radio.checked = true;

  for (const button of document.querySelectorAll('.copy')) {
    const code = button.parentElement.querySelector('code');
    if (!navigator.clipboard || !code) continue;
    button.hidden = false;
    button.addEventListener('click', async () => {
      await navigator.clipboard.writeText(code.textContent.trim());
      button.textContent = 'copied';
      setTimeout(() => { button.textContent = 'copy'; }, 1200);
    });
  }
})();
