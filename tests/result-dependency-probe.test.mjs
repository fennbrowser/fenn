import assert from 'node:assert/strict';
import test from 'node:test';
import {installResultDependencyProbe} from './browser/native-result-dependency-probe.mjs';

function fixture() {
  const methods = {
    prepareNativeResultMenu() { return Promise.resolve({}); },
    activateNativeResultMenu() { return this; },
    prepareNativeTipButtons() { return Promise.resolve([]); },
    prepareNativeTipDescription() { return Promise.resolve(null); },
    createDismissalAcknowledgment(result) { return result; },
  };
  const view = Object.create(methods);
  const window = {gURLBar: {view}, performance,
    document: {createElement: () => ({}), createElementNS: () => ({})}};
  return {window, view, methods};
}

test('observation preserves receiver, arguments, values and inherited descriptors', async () => {
  const {window, view, methods} = fixture();
  const probe = installResultDependencyProbe(window);
  assert.equal(view.activateNativeResultMenu(), view);
  assert.equal(await view.prepareNativeTipDescription(), null);
  const result = {type: 7};
  assert.equal(view.createDismissalAcknowledgment(result), result);
  assert.equal(probe.snapshot().samples.length, 3);
  probe.restore(); probe.restore();
  assert.equal(view.prepareNativeResultMenu, methods.prepareNativeResultMenu);
  assert.equal(Object.hasOwn(view, 'prepareNativeResultMenu'), false);
});

test('async rejection remains visible and pending work cannot pass the gate', async () => {
  const {window, view} = fixture();
  let reject;
  const failure = new Error('provider failure');
  view.prepareNativeResultMenu = () => new Promise((_, no) => reject = no);
  const original = view.prepareNativeResultMenu;
  const probe = installResultDependencyProbe(window);
  const pending = view.prepareNativeResultMenu();
  assert.throws(() => probe.assertIndependent(), /pending/);
  reject(failure);
  await assert.rejects(pending, error => error === failure);
  assert.equal(probe.snapshot().samples[0].outcome, 'rejected');
  assert.equal(probe.snapshot().pending, 0);
  probe.restore();
  assert.equal(view.prepareNativeResultMenu, original);
});

test('forbidden presenter use fails even when the caller catches the error', () => {
  const {window, view} = fixture();
  const probe = installResultDependencyProbe(window, 'forbid');
  for (const method of ['prepareNativeResultMenu', 'activateNativeResultMenu',
    'prepareNativeTipButtons', 'prepareNativeTipDescription']) {
    assert.throws(() => view[method](), /Retired result presenter/);
  }
  assert.throws(() => probe.assertIndependent(), /prohibition failed/);
  assert.equal(probe.snapshot().violations.length, 4);
  assert.equal(view.createDismissalAcknowledgment(7), 7);
  probe.restore();
});

test('partial installation failure restores every earlier hook', () => {
  const {window, view, methods} = fixture();
  window.document.createElementNS = undefined;
  assert.throws(() => installResultDependencyProbe(window), /Missing probe target/);
  assert.equal(view.prepareNativeResultMenu, methods.prepareNativeResultMenu);
  assert.equal(Object.hasOwn(view, 'prepareNativeResultMenu'), false);
});

test('prohibition remains usable after the legacy helpers are deleted', () => {
  const {window} = fixture();
  window.gURLBar.view = {};
  const probe = installResultDependencyProbe(window, 'forbid');
  probe.assertIndependent();
  assert.equal(Object.keys(window.gURLBar.view).length, 0);
  probe.restore();
});

test('observations stay bounded while counters continue counting', () => {
  const {window, view} = fixture();
  const probe = installResultDependencyProbe(window);
  for (let i = 0; i < 300; i++) view.createDismissalAcknowledgment({type: 7});
  const report = probe.snapshot();
  assert.equal(report.calls.createDismissalAcknowledgment, 300);
  assert.equal(report.samples.length, 256);
  assert.equal(report.droppedSamples, 44);
  probe.restore();
});

test('baseline descriptors retain localized ordering and command metadata without DOM nodes', async () => {
  const {window, view} = fixture();
  const node = {textContent: 'Localized action', dataset: {command: 'dismiss', input: 'new query'},
    getAttribute: () => 'message-id', querySelector: () => null};
  view.prepareNativeResultMenu = async () => ({children: [{localName: 'hr'}, node]});
  view.prepareNativeTipButtons = async () => [node];
  view.prepareNativeTipDescription = async () => ({before: 'Before ', after: ' after', label: 'Help', element: node});
  const probe = installResultDependencyProbe(window);
  await view.prepareNativeResultMenu();
  await view.prepareNativeTipButtons();
  await view.prepareNativeTipDescription();
  const samples = probe.snapshot().samples;
  assert.equal(samples[0].descriptor[0].separator, true);
  assert.equal(samples[0].descriptor[1].command, 'dismiss');
  assert.equal(samples[1].descriptor[0].input, 'new query');
  assert.deepEqual(samples[2].descriptor, {before: 'Before ', after: ' after', label: 'Help'});
  assert.equal(JSON.stringify(samples).includes('element'), false);
  probe.restore();
});
