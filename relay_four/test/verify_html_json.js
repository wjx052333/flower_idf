/**
 * verify_html_json.js — 验证 main.c 中 C-string HTML 与 JSON 格式的测试脚本
 *
 * 遵循 MEMORY.md 指南：
 * 1. 语法扫描提取 STATUS_HTML 的 C 字符串字面量（精确追踪转义符并在字符串外部的分号处终止）
 * 2. 反转义还原 HTML 页面，验证 4 路继电器卡片与控制路由
 * 3. 校验内嵌 <script> 脚本语法合法性
 * 4. 验证 /api/sensors 的 JSON 响应格式与字段完整性（支持转义字符串与宏常量格式）
 */

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const mainPath = path.resolve(__dirname, '../main/main.c');
if (!fs.existsSync(mainPath)) {
  console.error("FAIL: main.c does not exist yet");
  process.exit(1);
}

const mainContent = fs.readFileSync(mainPath, 'utf8');

/**
 * 提取 C 源码中声明的字符串变量内容
 * 遍历字符，处理转义字符，忽略注释，并在字符串外的首个 ';' 处终止
 */
function extractCStringDeclaration(content, varName) {
  const declRegex = new RegExp(`(?:static\\s+const\\s+char|const\\s+char)\\s+${varName}\\s*\\[\\s*\\]\\s*=`);
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
        // 遇到字符串外部的分号，变量声明结束
        break;
      } else if (ch === '"') {
        inString = true;
        currentLiteral = "";
        i++;
      } else {
        i++;
      }
    } else {
      // 字符串字面量内部
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

// 1. 提取 STATUS_HTML 内容
const unescapedHtml = extractCStringDeclaration(mainContent, "STATUS_HTML");
if (!unescapedHtml) {
  console.error("FAIL: STATUS_HTML declaration not found in main.c");
  process.exit(1);
}

// 验证 HTML 包含 4 路继电器的元素与路由
const requiredSnippets = [
  'id="relay-card-1"',
  'id="relay-card-2"',
  'id="relay-card-3"',
  'id="relay-card-4"',
  '/change_relay1',
  '/change_relay2',
  '/change_relay3',
  '/change_relay4',
  '/api/sensors'
];

for (const snippet of requiredSnippets) {
  if (!unescapedHtml.includes(snippet)) {
    console.error(`FAIL: HTML missing required element/route: ${snippet}`);
    process.exit(1);
  }
}

// 2. 校验内联 JavaScript 语法合法性
const scriptTags = [...unescapedHtml.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/gi)];
for (const [idx, sMatch] of scriptTags.entries()) {
  const jsCode = sMatch[1].trim();
  if (jsCode.length > 0) {
    try {
      new vm.Script(jsCode);
    } catch (err) {
      console.error(`FAIL: Embedded JavaScript syntax error in script tag #${idx + 1}: ${err.message}`);
      process.exit(1);
    }
  }
}

// 3. 检查 JSON 构造模板中的字段（兼容宏定义和字符串转义两种格式）
const hasJsonKey = (key) => mainContent.includes(`"${key}"`) || mainContent.includes(`\\"${key}\\"`);

if (!hasJsonKey("relays") ||
    !hasJsonKey("relay_last_on_ts") ||
    !hasJsonKey("relay_last_on_dur_ms")) {
  console.error("FAIL: JSON response format does not match specification");
  process.exit(1);
}

console.log("PASS: STATUS_HTML and API routes verification succeeded!");
