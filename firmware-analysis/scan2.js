const fs = require("fs");
const bytes = fs.readFileSync("C:/projects/ixus870IS/firmware-analysis/ixus870_sd880/sub/101a/PRIMARY.BIN");
const base = 0xFF810000;
function r32(o) { return bytes.readUInt32LE(o); }
function h(n) { return "0x" + (n>>>0).toString(16).toUpperCase().padStart(8,"0"); }

// The last BX LR before our area was at 0xFF8C1FE0
// So the function likely starts at 0xFF8C1FE4
// Look for PUSH right after 0xFF8C1FE0
console.log("=== Decode 0xFF8C1FD0 to 0xFF8C2010 ===");
for (let a = 0xFF8C1FD0; a < 0xFF8C2010; a += 4) {
  let i = r32(a - base);
  let d = "";
  if ((i & 0xFFFF0000) === 0xE92D0000) {
    let regs = [];
    for (let r = 0; r < 16; r++) if (i & (1<<r)) regs.push("R"+r);
    d = "PUSH {" + regs.join(",") + "}";
  } else if ((i & 0xFFFFF000) === 0xE24DD000) {
    d = "SUB SP, SP, #0x" + (i&0xFFF).toString(16).toUpperCase();
  } else if (i === 0xE12FFF1E) d = "BX LR";
  else if ((i & 0xFFFF0000) === 0xE8BD0000 && (i & 0x8000)) {
    let regs = [];
    for (let r = 0; r < 16; r++) if (i & (1<<r)) regs.push(r===15?"PC":"R"+r);
    d = "POP {" + regs.join(",") + "}";
  } else if ((i & 0x0E000000) === 0x02000000) {
    let op=(i>>>21)&0xF, rn=(i>>>16)&0xF, rd=(i>>>12)&0xF;
    let imm=i&0xFF, rot=((i>>>8)&0xF)*2;
    if (rot>0) imm=((imm>>>rot)|(imm<<(32-rot)))>>>0;
    let n=["AND","EOR","SUB","RSB","ADD","ADC","SBC","RSC","TST","TEQ","CMP","CMN","ORR","MOV","BIC","MVN"][op];
    if (op>=8&&op<=11) d=n+" R"+rn+", #0x"+imm.toString(16).toUpperCase();
    else if (op===13||op===15) d=n+" R"+rd+", #0x"+imm.toString(16).toUpperCase();
    else d=n+" R"+rd+", R"+rn+", #0x"+imm.toString(16).toUpperCase();
  } else if ((i&0x0C000000)===0x04000000) {
    let L=(i>>>20)&1,B=(i>>>22)&1,U=(i>>>23)&1;
    let nm=L?"LDR":"STR"; if(B)nm+="B";
    d=nm+" R"+((i>>>12)&0xF)+", [R"+((i>>>16)&0xF)+", #"+(U?"":"-")+"0x"+(i&0xFFF).toString(16).toUpperCase()+"]";
  } else if ((i&0x0E000000)===0x0A000000) {
    let o=i&0xFFFFFF; if(o>=0x800000)o-=0x1000000;
    d=((i&0x1000000)?"BL":"B")+" "+h(a+8+o*4);
  } else if ((i&0x0E000000)===0&&(i&0x90)!==0x90) {
    let op=(i>>>21)&0xF,rn=(i>>>16)&0xF,rd=(i>>>12)&0xF,rm=i&0xF;
    let n=["AND","EOR","SUB","RSB","ADD","ADC","SBC","RSC","TST","TEQ","CMP","CMN","ORR","MOV","BIC","MVN"][op];
    if (op===13) d="MOV R"+rd+", R"+rm;
    else d=n+" R"+rd+", R"+rn+", R"+rm;
  }
  if (!d) d = "raw 0x" + i.toString(16).toUpperCase().padStart(8,"0");
  console.log("  " + h(a) + ": " + h(i) + "  " + d);
}

// Now scan for end of function: 0xFF8C3100 to 0xFF8C4200 (the known stubs area)
console.log("");
console.log("=== Scanning 0xFF8C2F00 to 0xFF8C4300 for PUSH/POP/BX ===");
for (let a = 0xFF8C2F00; a < 0xFF8C4300; a += 4) {
  let i = r32(a - base);
  if ((i & 0xFFFF0000) === 0xE92D0000) {
    let regs = [];
    for (let r = 0; r < 16; r++) if (i & (1<<r)) regs.push("R"+r);
    console.log("  PUSH at " + h(a) + ": {" + regs.join(",") + "}");
  }
  if ((i & 0xFFFFF000) === 0xE24DD000) console.log("  SUB SP at " + h(a));
  if (i === 0xE12FFF1E) console.log("  BX LR at " + h(a));
  if ((i & 0xFFFF0000) === 0xE8BD0000 && (i & 0x8000)) {
    let regs = [];
    for (let r = 0; r < 16; r++) if (i & (1<<r)) regs.push(r===15?"PC":"R"+r);
    console.log("  POP+PC at " + h(a) + ": {" + regs.join(",") + "}");
  }
  if (i === 0xE1A0F00E) console.log("  MOV PC,LR at " + h(a));
}