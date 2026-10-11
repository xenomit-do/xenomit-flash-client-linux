const assert = require('assert');
const {encodeNavigation} = require('../src/navigation');
const now = Date.now();
const make = () => ({status:'ok', data:{observedAtUnixMs:now, fps:60, navigation:{map_id:6,geometry_ready:true,portals:[],obstacles:[
  {id:1,name:'circle',inverted:false,shape:{type:'circle',center:{x:10,y:20},radius:5}},
  {id:2,name:'rect',inverted:false,shape:{type:'rectangle',bounds:{x:1,y:2,width:10,height:20}}},
  {id:3,name:'polygon',inverted:true,shape:{type:'polygon',points:[{x:0,y:0},{x:10,y:0},{x:10,y:10}]}}
]}}});
const encode = value => encodeNavigation(JSON.stringify(value),now);
const nullable=make();nullable.data.navigation.star_system=null;nullable.data.navigation.hero={items:null};
const normalized=JSON.parse(encode(nullable).buffer.subarray(24));
assert(!('star_system' in normalized));assert(!('items' in normalized.hero));
let result=encode(make());
assert.strictEqual(result.buffer.readInt32LE(16),6);
assert.strictEqual(Number(result.buffer.readBigUInt64LE(8)),now);
assert.strictEqual(JSON.parse(result.buffer.subarray(24)).obstacles[2].inverted,true);
for (const mutate of [
  v=>v.data.observedAtUnixMs=now-751,
  v=>v.data.observedAtUnixMs=now+1001,
  v=>v.data.navigation.map_id=-1,
  v=>v.data.navigation.obstacles[0].shape.radius=NaN,
  v=>v.data.navigation.obstacles[1].shape.bounds.width=-1,
  v=>v.data.navigation.obstacles[2].shape.points=[{x:0,y:0}],
  v=>v.data.navigation.obstacles[0].shape.type='unknown',
  v=>v.data.navigation.obstacles=Array(257).fill(v.data.navigation.obstacles[0])
]) { const v=make(); mutate(v); assert.throws(()=>encode(v)); }
const empty=make(); empty.data.navigation.obstacles=[]; assert.strictEqual(encode(empty).metrics.geometryReady,true);
empty.data.navigation.geometry_ready=false; assert.strictEqual(encode(empty).metrics.geometryReady,false);
console.log('navigation shape, bounds and freshness tests passed');
