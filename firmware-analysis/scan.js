const fs = require("fs");
const bytes = fs.readFileSync("C:/projects/ixus870IS/firmware-analysis/ixus870_sd880/sub/101a/PRIMARY.BIN");
const base = 0xFF810000;
function r32(o) { return bytes.readUInt32LE(o); }
function h(n) { return "0x" + (n>>>0).toString(16).toUpperCase().padStart(8,"0"); }

console.log("=== Scanning 0xFF8C0800 to 0xFF8C2A00 ===");
for (let a = 0xFF8C0800; a < 0xFF8C2A00; a += 4) {
  let i = r32(a - base);
  if ((i & 0xFFFF0000) === 0xE92D0000) console.log("  PUSH at " + h(a) + ": " + h(i));
  if ((i & 0xFFFFF000) === 0xE24DD000) console.log("  SUB SP at " + h(a) + ": " + h(i));
  if (i === 0xE12FFF1E) console.log("  BX LR at " + h(a));
  if ((i & 0xFFFF0000) === 0xE8BD0000 && (i & 0x8000)) console.log("  POP+PC at " + h(a) + ": " + h(i));
  if (i === 0xE1A0F00E) console.log("  MOV PC,LR at " + h(a));
}

console.log("");
console.log("=== Scanning 0xFF8C2900 to 0xFF8C3100 ===");
for (let a = 0xFF8C2900; a < 0xFF8C3100; a += 4) {
  let i = r32(a - base);
  if ((i & 0xFFFF0000) === 0xE92D0000) console.log("  PUSH at " + h(a) + ": " + h(i));
  if ((i & 0xFFFFF000) === 0xE24DD000) console.log("  SUB SP at " + h(a) + ": " + h(i));
  if (i === 0xE12FFF1E) console.log("  BX LR at " + h(a));
  if ((i & 0xFFFF0000) === 0xE8BD0000 && (i & 0x8000)) console.log("  POP+PC at " + h(a) + ": " + h(i));
  if (i === 0xE1A0F00E) console.log("  MOV PC,LR at " + h(a));
}

// Switch table decode
console.log("");
console.log("=== Switch table at 0xFF8C2620 ===");
for (let a = 0xFF8C2618; a < 0xFF8C2650; a += 4) {
  let i = r32(a - base);
  let d = "";
  if ((i & 0x0E000000) === 0x0A000000) {
    let o = i & 0x00FFFFFF; if (o >= 0x800000) o -= 0x1000000;
    let t = a + 8 + o * 4;
    let l = (i & 0x01000000) ? "BL" : "B";
    d = l + " " + h(t);
  } else if ((i & 0x0FF00000) === 0x03500000) {
    d = "CMP R" + ((i>>>16)&0xF) + ", #" + (i&0xFF);
  } else if (i === 0x908FF100) {
    d = "ADDLS PC, PC, R0, LSL#2";
  } else if ((i & 0x0F700000) === 0x05900000) {
    d = "LDR R" + ((i>>>12)&0xF) + ", [R" + ((i>>>16)&0xF) + ", #0x" + (i&0xFFF).toString(16).toUpperCase() + "]";
  } else if ((i & 0x0F700000) === 0x05800000) {
    d = "STR R" + ((i>>>12)&0xF) + ", [R" + ((i>>>16)&0xF) + ", #0x" + (i&0xFFF).toString(16).toUpperCase() + "]";
  } else d = "raw";
  console.log("  " + h(a) + ": " + h(i) + "  " + d);
}

// Decode around 0xFF8C28C8
console.log("");
console.log("=== Decode 0xFF8C2890-0xFF8C2920 ===");
for (let a = 0xFF8C2890; a < 0xFF8C2920; a += 4) {
  let i = r32(a - base);
  let d = "";
  // imm data proc
  if ((i & 0x0E000000) === 0x02000000) {
    let op = (i>>>21)&0xF, rn=(i>>>16)&0xF, rd=(i>>>12)&0xF;
    let imm=i&0xFF, rot=((i>>>8)&0xF)*2;
    if (rot>0) imm = ((imm>>>rot)|(imm<<(32-rot)))>>>0;
    let n=["AND","EOR","SUB","RSB","ADD","ADC","SBC","RSC","TST","TEQ","CMP","CMN","ORR","MOV","BIC","MVN"][op];
    if (op>=8&&op<=11) d=n+" R"+rn+", #0x"+imm.toString(16).toUpperCase();
    else if (op===13||op===15) d=n+" R"+rd+", #0x"+imm.toString(16).toUpperCase();
    else d=n+" R"+rd+", R"+rn+", #0x"+imm.toString(16).toUpperCase();
  }
  // reg data proc (no mul)
  else if ((i&0x0E000000)===0&&(i&0x90)!==0x90) {
    let op=(i>>>21)&0xF,rn=(i>>>16)&0xF,rd=(i>>>12)&0xF,rm=i&0xF;
    let sh=(i>>>5)&3,sa=(i>>>7)&0x1F;
    let n=["AND","EOR","SUB","RSB","ADD","ADC","SBC","RSC","TST","TEQ","CMP","CMN","ORR","MOV","BIC","MVN"][op];
    let sn=["LSL","LSR","ASR","ROR"][sh];
    if (op===13) { d="MOV R"+rd+", R"+rm; if(sa)d+=", "+sn+" #"+sa; }
    else if (op>=8&&op<=11) d=n+" R"+rn+", R"+rm;
    else { d=n+" R"+rd+", R"+rn+", R"+rm; if(sa)d+=", "+sn+" #"+sa; }
  }
  // MUL
  else if ((i&0x0FC000F0)===0x00000090) {
    d="MUL R"+((i>>>16)&0xF)+", R"+(i&0xF)+", R"+((i>>>8)&0xF);
  }
  // LDR/STR imm
  else if ((i&0x0C000000)===0x04000000) {
    let L=(i>>>20)&1,B=(i>>>22)&1,U=(i>>>23)&1;
    let nm=L?"LDR":"STR"; if(B)nm+="B";
    d=nm+" R"+((i>>>12)&0xF)+", [R"+((i>>>16)&0xF)+", #"+(U?"":"-")+"0x"+(i&0xFFF).toString(16).toUpperCase()+"]";
  }
  // Branch
  else if ((i&0x0E000000)===0x0A000000) {
    let o=i&0xFFFFFF; if(o>=0x800000)o-=0x1000000;
    let t=a+8+o*4;
    d=((i&0x1000000)?"BL":"B")+" "+h(t);
  }
  // Halfword
  else if ((i&0x0E000090)===0x90&&(i&0x60)) {
    let L=(i>>>20)&1, sh2=(i>>>5)&3;
    let imm2=((i>>>4)&0xF0)|(i&0xF);
    let nm2=L?"LDR":"STR"; nm2+=["?","H","SB","SH"][sh2];
    d=nm2+" R"+((i>>>12)&0xF)+", [R"+((i>>>16)&0xF)+", #0x"+imm2.toString(16).toUpperCase()+"]";
  }
  if (!d) d = "raw 0x" + i.toString(16).toUpperCase().padStart(8,"0");
  console.log("  " + h(a) + ": " + h(i) + "  " + d);
}