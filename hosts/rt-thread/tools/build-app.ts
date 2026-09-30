/** RT-Thread adapter. Shared compiler and .pocket wire format remain unchanged. */
import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { validateAndResolveBuildPlan } from "../../../framework/src/manifest/resolve.ts";
import { canonicalJson } from "../../../framework/src/manifest/plan.ts";
import { createHostExtension } from "../../../framework/src/manifest/host-extension.ts";
import { POCKET_CAPABILITIES, definePlatformContractRegistry, defineTargetRegistry } from "../../../contracts/spec/platforms.ts";
import { encodePocketPackage, encodeHostInputs, POCKET_SECTION, decodePocketPackage } from "../../../contracts/spec/pocket-package.ts";
import { makeVariant } from "../../../tools/pocket-pack.ts";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../../..");
const example = resolve(root, "hosts/rt-thread/examples/counter");
const out = resolve(root, ".pocket-build/d13x/counter");
const profile = JSON.parse(readFileSync(resolve(example, "pocket.host.json"), "utf8"));
// This adapter currently admits only the validated D13x smoke presentation.
if (profile.version !== 1 || profile.platform !== "rt-thread" || profile.id !== "d13x-smoke" ||
    profile.tickHz !== 60 || profile.form !== "embedded" ||
    canonicalJson(profile.display) !== canonicalJson({ physicalViewport:[800,480], logicalViewports:[[800,480]], presentations:["native"], rasterDensity:1 }) ||
    canonicalJson(profile.capabilities) !== canonicalJson(["text.glyphs.baked"]))
  throw new Error("RT-Thread profile is outside the validated D13x smoke contract");
const manifestBytes = readFileSync(resolve(example, "pocket.json"));
const manifest = JSON.parse(manifestBytes.toString("utf8"));
const profileHash = "sha256:" + createHash("sha256").update(canonicalJson(profile)).digest("hex");
const registry = definePlatformContractRegistry(POCKET_CAPABILITIES, defineTargetRegistry({
  [profile.id]: { hostAbi:1, platform:profile.platform, form:profile.form, display:profile.display, capabilities:profile.capabilities },
}));
const result = validateAndResolveBuildPlan(manifest, {
  target:profile.id,
  hostExtension:createHostExtension("rt-thread", 1, {profileHash, tickHz:profile.tickHz}),
}, registry);
if (!result.ok) throw new Error(JSON.stringify(result.diagnostics));
const plan = result.plan;
mkdirSync(out, {recursive:true});
const planFile = resolve(out, "plan.json");
writeFileSync(planFile, canonicalJson(plan));
const compile = Bun.spawnSync([process.execPath, resolve(root, "tools/build.ts"),
  "--plan="+planFile, "--project-root="+example, "--outdir="+out,
  "--hz="+profile.tickHz, "--extra-chars=0123456789", "--no-config"],
  {cwd:root, stdout:"inherit", stderr:"inherit"});
if (compile.exitCode) process.exit(compile.exitCode);
const variant = makeVariant({target:profile.id, hostAbi:1, planJson:canonicalJson(plan),
  identity:{output:plan.app.output,id:plan.app.id,title:plan.app.title},
  js:readFileSync(resolve(out, plan.app.output+".js")),
  pak:readFileSync(resolve(out, plan.app.output+".pak"))});
variant.sections.push({kind:POCKET_SECTION.hostInputs, bytes:encodeHostInputs({
  hostAbi:1, tickHz:profile.tickHz, logicalWidth:plan.viewport.logical[0], logicalHeight:plan.viewport.logical[1],
  physicalWidth:plan.viewport.physical[0], physicalHeight:plan.viewport.physical[1],
  rasterDensity:plan.viewport.rasterDensity, presentation:plan.viewport.presentation, profileHash, planHash:plan.planHash,
})});
const bytes = encodePocketPackage({manifest:manifestBytes, variants:[variant]});
decodePocketPackage(bytes); // Verify the container footer/table before embedding.
const destination = resolve(out, "counter.pocket");
writeFileSync(destination, bytes);
console.log(JSON.stringify({package:destination, bytes:bytes.length, target:profile.id, profileHash, planHash:plan.planHash}));
