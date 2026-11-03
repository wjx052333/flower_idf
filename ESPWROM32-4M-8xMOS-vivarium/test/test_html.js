const fs = require('fs');
const path = require('path');
const vm = require('vm');

const targetFile = process.argv[2] || '../main/main.c';
const mainPath = path.resolve(__dirname, targetFile);
const mainContent = fs.readFileSync(mainPath, 'utf8');

function extractCStringDeclaration(content, varName) {
  const declRegex = new RegExp('(?:static\\s+const\\s+char|const\\s+char)\\s+' + varName + '\\s*\\[\\s*\\d*\\s*\\]\\s*=');
  const match = content.match(declRegex);
  if (!match) return null;

  const startIdx = match.index + match[0].length;
  let i = startIdx;
  let inString = false;
  let inLineComment = false;
  let inBlockComment = false;
  let currentLiteral = "";
  let fullStr = "";

  while (i < content.length) {
    const ch = content[i];
    const next = content[i + 1];

    if (inLineComment) {
      if (ch === '\n') inLineComment = false;
      i++;
    } else if (inBlockComment) {
      if (ch === '*' && next === '/') {
        inBlockComment = false;
        i += 2;
      } else {
        i++;
      }
    } else if (!inString) {
      if (ch === '/' && next === '/') {
        inLineComment = true;
        i += 2;
      } else if (ch === '/' && next === '*') {
        inBlockComment = true;
        i += 2;
      } else if (ch === ';') {
        break;
      } else if (ch === '"') {
        inString = true;
        currentLiteral = "";
        i++;
      } else {
        i++;
      }
    } else {
      if (ch === '\\') {
        const esc = content[i + 1];
        if (esc === 'n') currentLiteral += '\n';
        else if (esc === 'r') currentLiteral += '\r';
        else if (esc === 't') currentLiteral += '\t';
        else if (esc === '"') currentLiteral += '"';
        else if (esc === '\\') currentLiteral += '\\';
        else currentLiteral += '\\' + esc;
        i += 2;
      } else if (ch === '"') {
        inString = false;
        fullStr += currentLiteral;
        i++;
      } else {
        currentLiteral += ch;
        i++;
      }
    }
  }

  return fullStr;
}

const html = extractCStringDeclaration(mainContent, 'STATUS_HTML');
if (!html) {
  console.error('FAIL: STATUS_HTML not found!');
  process.exit(1);
}

// 1. Tag balance checks
const checkTagBalance = (str, tag) => {
  const openCount = (str.match(new RegExp(`<${tag}\\b`, 'gi')) || []).length;
  const closeCount = (str.match(new RegExp(`</${tag}>`, 'gi')) || []).length;
  if (openCount !== closeCount) {
    throw new Error(`Tag mismatch for <${tag}>: open=${openCount}, close=${closeCount}`);
  }
};
['div', 'script', 'style', 'html', 'head', 'body'].forEach(t => checkTagBalance(html, t));
console.log('PASS: All HTML tags balanced.');

// 2. JS extraction and validation
const scripts = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/gi)];
scripts.forEach((s, idx) => {
  const js = s[1].trim();
  try {
    new vm.Script(js);
    console.log(`PASS: Script #${idx + 1} syntax is valid!`);
  } catch (e) {
    console.error(`FAIL: Script #${idx + 1} syntax error: ` + e.message);
    process.exit(1);
  }

  // 3. Check element IDs referenced in JS exist in HTML
  const idMatches = [...js.matchAll(/getElementById\(['"]([^'"]+)['"]\)/g)];
  idMatches.forEach(m => {
    const id = m[1];
    if (!id.includes('+') && !html.includes(`id="${id}"`)) {
      throw new Error(`Element id="${id}" referenced in JS but not found in HTML!`);
    }
  });
});

console.log('PASS: All referenced element IDs exist in HTML.');

// 4. Check JSON keys in mainContent
const requiredJsonKeys = ["soil_raw", "soil_hum", "air_temp", "air_rh", "slab_temp", "soil_raw_dry", "soil_raw_wet"];
for (const key of requiredJsonKeys) {
  if (!mainContent.includes(`"${key}"`) && !mainContent.includes(`\\"${key}\\"`)) {
    throw new Error(`FAIL: main.c missing required key: ${key}`);
  }
}
console.log('PASS: All required JSON keys present in main.c.');

console.log('ALL CHECKS PASSED!');
