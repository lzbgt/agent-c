#pragma once

namespace host_tools_internal {

// Fixed read-only browser code. Returned website text is untrusted source data,
// never an instruction to Agent C. Native code also enforces byte/URL limits.
inline constexpr const char* kBrowserSnapshotScript = R"AGENTDOM(
(() => {
  const LIMIT = {text: 32000, title: 512, snippet: 1000, url: 4096};
  const clip = (value, limit) => {
    let out = String(value == null ? '' : value).slice(0, limit);
    const last = out.charCodeAt(out.length - 1);
    if (last >= 0xD800 && last <= 0xDBFF) out = out.slice(0, -1);
    return out;
  };
  const clean = value => String(value == null ? '' : value).replace(/\s+/g, ' ').trim();
  const visible = node => {
    if (!node || !node.isConnected || node.closest('[hidden],[aria-hidden="true"],template,script,style,noscript')) return false;
    let current = node;
    for (let depth = 0; current && depth < 32; ++depth, current = current.parentElement) {
      const style = getComputedStyle(current);
      if (style.display === 'none' || style.visibility === 'hidden' ||
          style.visibility === 'collapse' || Number(style.opacity) === 0) return false;
    }
    return Array.from(node.getClientRects()).slice(0, 8).some(rect => rect.width > 0 && rect.height > 0);
  };
  const hasVisible = selector => Array.from(document.querySelectorAll(selector)).slice(0, 100).some(visible);
  const rendered = node => node ? String(node.innerText || '') : '';
  const host = location.hostname.toLowerCase();
  const googleHost = name => /(^|\.)google\.(?:com|[a-z]{2}|(?:co|com)\.[a-z]{2})$/.test(name);
  const bingHost = name => /(^|\.)bing\.com$/.test(name);
  const engine = googleHost(host) ? 'google' : bingHost(host) ? 'bing' :
    /(^|\.)search\.yahoo\.com$/.test(host) ? 'yahoo' :
    /(^|\.)mojeek\.com$/.test(host) ? 'mojeek' :
    /(^|\.)duckduckgo\.com$/.test(host) ? 'duckduckgo' :
    /(^|\.)baidu\.com$/.test(host) ? 'baidu' : '';

  // Only unwrap documented engine-shaped links on the corresponding host.
  const destination = raw => {
    if (!raw || String(raw).length > 8192) return '';
    try {
      let url = new URL(raw, location.href);
      if (googleHost(url.hostname) && url.pathname === '/url') {
        const target = url.searchParams.get('q') || url.searchParams.get('url');
        if (!target || target.length > LIMIT.url) return '';
        url = new URL(target);
      } else if (bingHost(url.hostname) && /^\/ck\/a\/?$/.test(url.pathname)) {
        const target = url.searchParams.get('u') || '';
        if (!target.startsWith('a1') || target.length > 6000) return '';
        let encoded = target.slice(2).replace(/-/g, '+').replace(/_/g, '/');
        encoded += '='.repeat((4 - encoded.length % 4) % 4);
        const bytes = Uint8Array.from(atob(encoded), c => c.charCodeAt(0));
        url = new URL(new TextDecoder('utf-8', {fatal: true}).decode(bytes));
      } else if (/(^|\.)r\.search\.yahoo\.com$/.test(url.hostname)) {
        const match = url.pathname.match(/\/RU=([^/]+)(?:\/|$)/);
        if (!match || match[1].length > 8192) return '';
        url = new URL(decodeURIComponent(match[1]));
      } else if (/(^|\.)duckduckgo\.com$/.test(url.hostname) && url.pathname === '/l/') {
        const target = url.searchParams.get('uddg');
        if (!target || target.length > LIMIT.url) return '';
        url = new URL(target);
      }
      if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password ||
          url.href.length > LIMIT.url || /[\u0000-\u0020\u007F]/.test(url.href)) return '';
      return url.href;
    } catch (_) { return ''; }
  };

  const results = [];
  const resultUrls = new Set();
  const addResult = (heading, anchor, container, snippetSelector) => {
    if (results.length >= 20 || !visible(heading) || !visible(anchor) ||
        (container && !visible(container))) return;
    const url = destination(anchor.getAttribute('href'));
    const title = clip(clean(rendered(heading)), LIMIT.title);
    if (!url || !title || resultUrls.has(url)) return;
    const parsed = new URL(url);
    if (parsed.hostname === host && /^(?:\/search|\/url|\/ck\/a)(?:\/|$)/.test(parsed.pathname)) return;
    let snippetNode = null;
    if (container && snippetSelector) {
      snippetNode = Array.from(container.querySelectorAll(snippetSelector)).slice(0, 20).find(visible);
    }
    let snippet = clean(rendered(snippetNode || container));
    if (snippet.startsWith(title)) snippet = snippet.slice(title.length).trim();
    resultUrls.add(url);
    results.push({title, url, snippet: clip(snippet, LIMIT.snippet)});
  };
  if (engine === 'google') {
    for (const heading of Array.from(document.querySelectorAll('#search h3, #rso h3, #main h3')).slice(0, 100)) {
      const anchor = heading.closest('a');
      const card = heading.closest('.MjjYud, .g, .tF2Cxc') || (anchor && anchor.parentElement);
      addResult(heading, anchor, card, '.VwiC3b, .IsZvec, [data-sncf]');
    }
  } else if (engine === 'bing') {
    for (const card of Array.from(document.querySelectorAll('li.b_algo')).slice(0, 100)) {
      const anchor = card.querySelector('h2 a');
      addResult(anchor, anchor, card, '.b_caption p, p.b_lineclamp2, p');
    }
  } else if (engine === 'yahoo') {
    for (const heading of Array.from(document.querySelectorAll('#web h3')).slice(0, 100)) {
      const anchor = heading.querySelector('a') || heading.closest('a');
      const card = heading.closest('.algo, .dd.algo, li') || heading.parentElement;
      addResult(heading, anchor, card, '.compText, .fc-falcon, p');
    }
  } else if (engine === 'mojeek') {
    for (const anchor of Array.from(document.querySelectorAll('.results-standard li h2 a, .results-standard li a.title')).slice(0, 100)) {
      addResult(anchor, anchor, anchor.closest('li'), 'p.s, p.snippet, .snippet, p');
    }
  } else if (engine === 'duckduckgo') {
    for (const anchor of Array.from(document.querySelectorAll('a.result__a, a.result-link, article[data-testid="result"] h2 a')).slice(0, 100)) {
      addResult(anchor, anchor, anchor.closest('.result, article, tr'), '.result__snippet, [data-result="snippet"], p');
    }
  } else if (engine === 'baidu') {
    for (const anchor of Array.from(document.querySelectorAll('.result h3 a, .result-op h3 a, #content_left .c-container h3 a')).slice(0, 100)) {
      addResult(anchor, anchor, anchor.closest('.result, .result-op, .c-container'), '.c-abstract, .content-right_8Zs40, p');
    }
  }

  const body = document.body;
  const main = Array.from(document.querySelectorAll('main,[role="main"],article')).slice(0, 30)
    .find(node => visible(node) && rendered(node).trim().length > 0) || body;
  const fullText = rendered(main).trim();
  const bodyStart = clean(rendered(body)).slice(0, 6000);
  const title = clip(clean(document.title), LIMIT.title);
  let reason = '';
  // Match actual gate markers, never an arbitrary article mentioning CAPTCHA.
  if (googleHost(host) && /^\/sorry(?:\/|$)/.test(location.pathname)) reason = 'google_unusual_traffic';
  else if (engine === 'baidu' && /^(?:百度安全验证|百度访问验证)$/.test(title)) reason = 'baidu_verification_required';
  else if (/^Oh noes!$/i.test(title) && /^Oh noes!\s+Access Denied\b/i.test(bodyStart) &&
           /Protected by Anubis\b/i.test(bodyStart)) reason = 'anubis_access_denied';
  else if (/^Making sure (?:you are|you['’]re) not a bot!$/i.test(title) &&
           /Protected by Anubis\b/i.test(bodyStart)) reason = 'anubis_verification_required';
  else if (host === 'consent.google.com' && hasVisible('form,button')) reason = 'consent_required';
  else if (hasVisible('#challenge-form,#cf-challenge-running,#cf-please-wait,#px-captcha,#challenge-stage')) reason = 'verification_required';
  else if (results.length === 0 && bodyStart.length < 6000 &&
           /^(?:just a moment|access denied|403 forbidden|forbidden|robot check|human verification|attention required)(?:\s*[.!|—-]|$)/i.test(title)) reason = 'access_or_verification_gate';
  else if (results.length === 0 &&
           /^(?:captcha|verify (?:that )?you are human|security verification)\s*[.!]?$/i.test(title) &&
           hasVisible('iframe[src*="recaptcha"],iframe[src*="hcaptcha"],.g-recaptcha,.h-captcha')) reason = 'captcha_required';
  else if (hasVisible('form input[type="password"]') &&
           (/^(?:sign in|log in|login) to (?:continue|view|access)\b/i.test(title) ||
            /^(?:sign in|log in|login) to (?:continue|view|access)\b/i.test(bodyStart))) reason = 'login_required';

  const noResultSelectors = {
    google: '#topstuff', bing: 'li.b_no, #b_results .b_no',
    yahoo: '#web .compNoResults, #web #noResults',
    mojeek: '.results .no-results, #results .no-results',
    duckduckgo: '.no-results, .no-results__message'
  };
  const noResults = !!engine && !reason && results.length === 0 &&
    Array.from(document.querySelectorAll(noResultSelectors[engine] || ':not(*)')).slice(0, 20).some(node =>
      visible(node) && /(?:no results (?:found|for)|did not match any documents|there are no results|did not find any results)/i.test(rendered(node)));

  const links = [];
  const linkUrls = new Set();
  for (const anchor of Array.from(document.querySelectorAll('a[href]')).slice(0, 500)) {
    if (links.length >= 30) break;
    if (!visible(anchor)) continue;
    const raw = anchor.getAttribute('href');
    if (!raw || raw.startsWith('#')) continue;
    const url = destination(raw);
    const label = clip(clean(rendered(anchor) || anchor.getAttribute('aria-label') || anchor.getAttribute('title')), LIMIT.title);
    if (!url || !label || linkUrls.has(url)) continue;
    linkUrls.add(url);
    links.push({title: label, url});
  }
  return {
    url: location.href.length <= LIMIT.url ? location.href : '', title,
    ready: document.readyState !== 'loading' && !!body,
    content_type: clip(document.contentType || '', 128),
    text: clip(fullText, LIMIT.text), text_length: fullText.length,
    text_truncated: fullText.length > LIMIT.text,
    challenge: !!reason, challenge_reason: reason,
    search_engine: engine, search_engine_detected: !!engine,
    no_results: noResults,
    search_results: reason ? [] : results,
    links: reason ? [] : links
  };
})()
)AGENTDOM";

}  // namespace host_tools_internal
