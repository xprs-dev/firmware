// XPRS crypto for the hotspot chat page: sha256, secp256k1 (native BigInt),
// the section 9.1.2 short-Schnorr, base85 and bech32. No dependencies -- a
// captive-portal WebView has no secure context and no WebCrypto.subtle.

// ---- sha256 (bytes in, bytes out) ----
function sha256(data){
  var K=[0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
  0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
  0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
  0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
  0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2];
  var H=[0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19];
  var l=data.length, buf=new Uint8Array(((l+9+63)>>6)<<6);
  buf.set(data); buf[l]=0x80;
  var bl=l*8;
  buf[buf.length-4]=(bl>>>24)&255; buf[buf.length-3]=(bl>>>16)&255;
  buf[buf.length-2]=(bl>>>8)&255; buf[buf.length-1]=bl&255;
  var w=new Array(64);
  for(var off=0;off<buf.length;off+=64){
    for(var i=0;i<16;i++) w[i]=(buf[off+4*i]<<24)|(buf[off+4*i+1]<<16)|(buf[off+4*i+2]<<8)|buf[off+4*i+3];
    for(i=16;i<64;i++){
      var a1=w[i-15], b1=w[i-2];
      var s0=((a1>>>7)|(a1<<25))^((a1>>>18)|(a1<<14))^(a1>>>3);
      var s1=((b1>>>17)|(b1<<15))^((b1>>>19)|(b1<<13))^(b1>>>10);
      w[i]=(w[i-16]+s0+w[i-7]+s1)|0;
    }
    var a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
    for(i=0;i<64;i++){
      var S1=((e>>>6)|(e<<26))^((e>>>11)|(e<<21))^((e>>>25)|(e<<7));
      var ch=(e&f)^(~e&g);
      var t1=(h+S1+ch+K[i]+w[i])|0;
      var S0=((a>>>2)|(a<<30))^((a>>>13)|(a<<19))^((a>>>22)|(a<<10));
      var mj=(a&b)^(a&c)^(b&c);
      var t2=(S0+mj)|0;
      h=g;g=f;f=e;e=(d+t1)|0;d=c;c=b;b=a;a=(t1+t2)|0;
    }
    H[0]=(H[0]+a)|0;H[1]=(H[1]+b)|0;H[2]=(H[2]+c)|0;H[3]=(H[3]+d)|0;
    H[4]=(H[4]+e)|0;H[5]=(H[5]+f)|0;H[6]=(H[6]+g)|0;H[7]=(H[7]+h)|0;
  }
  var out=new Uint8Array(32);
  for(i=0;i<8;i++){out[4*i]=(H[i]>>>24)&255;out[4*i+1]=(H[i]>>>16)&255;out[4*i+2]=(H[i]>>>8)&255;out[4*i+3]=H[i]&255;}
  return out;
}

// ---- byte/bigint helpers ----
function b2n(b){var v=0n;for(var i=0;i<b.length;i++)v=(v<<8n)|BigInt(b[i]);return v;}
function n2b(v,len){var out=new Uint8Array(len);for(var i=len-1;i>=0;i--){out[i]=Number(v&255n);v>>=8n;}return out;}
function cat(){var n=0,i,a=arguments;for(i=0;i<a.length;i++)n+=a[i].length;
  var out=new Uint8Array(n),o=0;for(i=0;i<a.length;i++){out.set(a[i],o);o+=a[i].length;}return out;}
function s2b(s){var out=new Uint8Array(s.length);for(var i=0;i<s.length;i++)out[i]=s.charCodeAt(i)&255;return out;}
function hex(b){var s='';for(var i=0;i<b.length;i++)s+=('0'+b[i].toString(16)).slice(-2);return s;}

// ---- secp256k1 (Jacobian double-and-add; one-shot signing, speed is fine) ----
var P=2n**256n-2n**32n-977n;
var N=0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141n;
var GX=0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798n;
var GY=0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8n;
function mod(a,m){var r=a%m;return r<0n?r+m:r;}
function powmod(b,e,m){var r=1n;b=mod(b,m);while(e>0n){if(e&1n)r=r*b%m;b=b*b%m;e>>=1n;}return r;}
function inv(a,m){return powmod(mod(a,m),m-2n,m);}
// Jacobian points [X,Y,Z]; infinity Z=0
function jdbl(p){var X=p[0],Y=p[1],Z=p[2];if(Z===0n)return p;
  var A=X*X%P,B=Y*Y%P,C=B*B%P;
  var D=mod(2n*((X+B)*(X+B)%P-A-C),P);
  var E=3n*A%P,F=E*E%P;
  var X3=mod(F-2n*D,P);
  return [X3,mod(E*(D-X3)-8n*C,P),mod(2n*Y*Z,P)];}
function jadd(p,q){if(p[2]===0n)return q;if(q[2]===0n)return p;
  var Z1Z1=p[2]*p[2]%P,Z2Z2=q[2]*q[2]%P;
  var U1=p[0]*Z2Z2%P,U2=q[0]*Z1Z1%P;
  var S1=p[1]*Z2Z2%P*q[2]%P,S2=q[1]*Z1Z1%P*p[2]%P;
  if(U1===U2)return S1===S2?jdbl(p):[0n,1n,0n];
  var H=mod(U2-U1,P),I=4n*H*H%P,J=H*I%P,r=mod(2n*(S2-S1),P),V=U1*I%P;
  var X3=mod(r*r-J-2n*V,P);
  return [X3,mod(r*(V-X3)-2n*S1*J,P),mod(2n*H*p[2]%P*q[2],P)];}
function jmul(k,px,py){var r=[0n,1n,0n],q=[px,py,1n];
  while(k>0n){if(k&1n)r=jadd(r,q);q=jdbl(q);k>>=1n;}return r;}
function jaff(p){if(p[2]===0n)return null;var zi=inv(p[2],P),zi2=zi*zi%P;
  return [p[0]*zi2%P,p[1]*zi2%P*zi%P];}
function mulG(k){return jaff(jmul(mod(k,N),GX,GY));}

// ---- tagged hash: sha256(sha256(tag)||sha256(tag)||msg) ----
function tagged(tag,msg){var th=sha256(s2b(tag));return sha256(cat(th,th,msg));}

// ---- XPRS short-Schnorr (9.1.2) ----
function xprsSign(canonicalStr,dPriv,aux){
  var digest=sha256(s2b(canonicalStr));
  var d=mod(dPriv,N); if(d===0n) throw 'bad key';
  var Pt=mulG(d);
  if(Pt[1]&1n){d=N-d;Pt=mulG(d);}
  var px=n2b(Pt[0],32);
  var k=b2n(tagged('XPRS/nonce',cat(n2b(d,32),digest,aux)));
  k=mod(k,N); if(k===0n)k=1n;
  var R=mulG(k);
  var rx=n2b(R[0],32);
  var e32=tagged('XPRS/challenge',cat(rx,px,digest));
  var e=b2n(e32.slice(0,16));
  var s=mod(k+e*d,N);
  return cat(e32.slice(0,16),n2b(s,32));
}

// ---- base85, the spec alphabet ----
var B85='0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-+=^!/*?&<>()[]%$#@,;_';
function b85enc(bytes){var out='';
  for(var i=0;i<bytes.length;i+=4){
    var v=(bytes[i]*16777216)+(bytes[i+1]*65536)+(bytes[i+2]*256)+bytes[i+3];
    var d=['','','','',''];
    for(var j=4;j>=0;j--){d[j]=B85[v%85];v=Math.floor(v/85);}
    out+=d.join('');}
  return out;}

// ---- bech32 (npub/nsec) ----
var BC='qpzry9x8gf2tvdw0s3jn54khce6mua7l';
function bcPolymod(vals){var GEN=[0x3b6a57b2,0x26508e6d,0x1ea119fa,0x3d4233dd,0x2a1462b3];
  var chk=1;for(var i=0;i<vals.length;i++){var top=chk>>25;chk=((chk&0x1ffffff)<<5)^vals[i];
  for(var j=0;j<5;j++)if((top>>j)&1)chk^=GEN[j];}return chk;}
function bcHrpExpand(hrp){var out=[];var i;for(i=0;i<hrp.length;i++)out.push(hrp.charCodeAt(i)>>5);
  out.push(0);for(i=0;i<hrp.length;i++)out.push(hrp.charCodeAt(i)&31);return out;}
function bech32Encode(hrp,data5){var vals=bcHrpExpand(hrp).concat(data5);
  var pm=bcPolymod(vals.concat([0,0,0,0,0,0]))^1;var chk='';
  for(var i=0;i<6;i++)chk+=BC[(pm>>(5*(5-i)))&31];
  var s=hrp+'1';for(i=0;i<data5.length;i++)s+=BC[data5[i]];return s+chk;}
function bech32Decode(str){str=str.toLowerCase();var pos=str.lastIndexOf('1');
  if(pos<1||pos+7>str.length)return null;
  var hrp=str.slice(0,pos),data=[];
  for(var i=pos+1;i<str.length;i++){var d=BC.indexOf(str[i]);if(d<0)return null;data.push(d);}
  if(bcPolymod(bcHrpExpand(hrp).concat(data))!==1)return null;
  return {hrp:hrp,data:data.slice(0,-6)};}
function to5(bytes){var out=[],acc=0,bits=0;
  for(var i=0;i<bytes.length;i++){acc=(acc<<8)|bytes[i];bits+=8;
    while(bits>=5){bits-=5;out.push((acc>>bits)&31);}}
  if(bits>0)out.push((acc<<(5-bits))&31);return out;}
function to8(data5){var out=[],acc=0,bits=0;
  for(var i=0;i<data5.length;i++){acc=(acc<<5)|data5[i];bits+=5;
    while(bits>=8){bits-=8;out.push((acc>>bits)&255);}}
  return new Uint8Array(out);}
function npubEncode(pubx){return bech32Encode('npub',to5(pubx));}
function nsecEncode(priv){return bech32Encode('nsec',to5(priv));}
function nsecDecode(str){var d=bech32Decode(str);
  if(!d||d.hrp!=='nsec')return null;var b=to8(d.data);
  return b.length===32?b:null;}

// ---- identity helpers ----
function callsignFromNpub(npub){var s='X3';
  for(var i=5;i<9;i++)s+=npub[i].toUpperCase();return s;}
function pubkeyX(privBytes){var Pt=mulG(b2n(privBytes));return n2b(Pt[0],32);}

// ---- node test against the spec 9.1.2 worked example ----
if(typeof module!=='undefined'&&typeof require!=='undefined'&&require.main===module){
  var canonical='t:message f:X1QZ3N d:LISBOA ts:2026-08-08_14:26:40 m:net starts in ten minutes';
  var d=7n, aux=new Uint8Array(32);
  var dg=sha256(s2b(canonical));
  console.log('digest', hex(dg), hex(dg)==='39922745225b987201d0a253ed152b99712088ba6c578a41bdfc670594a3c553'?'OK':'FAIL');
  var sig=xprsSign(canonical,d,aux);
  var want='b40348c6defc8e1ae6dfca7635513e3be052dd3b72c2aab12db5d39d047de1816f15f396a402ea9d2d3407bde0dd5df8';
  console.log('sig   ', hex(sig), hex(sig)===want?'OK':'FAIL');
  var b85=b85enc(sig);
  console.log('b85   ', b85, b85==='V<-(s&U-xL(hjs8hbML0<8nw[A)a<YeW+5_1BYlWzX.)fQYP&LeI[ZC<n4Yl'?'OK':'FAIL');
  var pk=pubkeyX(n2b(7n,32));
  console.log('px    ', hex(pk), hex(pk)==='5cbdf0646e5db4eaa398f365f2ea7a0e3d419b7e0330e39ce92bddedcac4f9bc'?'OK':'FAIL');
  var np=npubEncode(pk);
  console.log('npub  ', np, 'callsign', callsignFromNpub(np));
  var ns=nsecEncode(n2b(7n,32));
  var back=nsecDecode(ns);
  console.log('nsec roundtrip', ns.slice(0,12)+'...', back&&b2n(back)===7n?'OK':'FAIL');
}
