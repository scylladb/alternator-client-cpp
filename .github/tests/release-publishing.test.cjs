// Copyright ScyllaDB, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

const assert = require('node:assert/strict');
const {readFileSync, mkdtempSync, writeFileSync, chmodSync, rmSync} = require('node:fs');
const {tmpdir} = require('node:os');
const {join, resolve} = require('node:path');
const {spawnSync} = require('node:child_process');
const {test} = require('node:test');

const root = resolve(__dirname, '../..');
const releaseWorkflow = readFileSync(join(root, '.github/workflows/release.yml'), 'utf8');
const releaseScript = releaseWorkflow.split(/^      - name: Create GitHub Release\n/m)[1]
  .split(/^          script: \|\n/m)[1].split(/\n(?= {0,11}\S)/, 1)[0]
  .split('\n').map(line => line.replace(/^            /, '')).join('\n');
const publishRelease = new (Object.getPrototypeOf(async function () {}).constructor)(
  'github', 'context', 'process', 'core', releaseScript
);

test('existing release is preserved; missing release uses generated notes; API errors fail', async () => {
  for (const status of [200, 'draft', 404, 403, 500]) {
    const calls = [];
    const github = {paginate: async (method, params) => {
      assert.equal(method, github.rest.repos.listReleases);
      assert.deepEqual(params, {owner: 'scylladb', repo: 'alternator-client-cpp', per_page: 100});
      return status === 'draft' ? [{tag_name: 'v1.2.3', draft: true}] : [];
    }, rest: {git: {getRef: async params => {
      assert.deepEqual(params, {owner: 'scylladb', repo: 'alternator-client-cpp', ref: 'tags/v1.2.3'});
      return {data: {object: {sha: 'verified-tag-object'}}};
    }}, repos: {
      listReleases: () => {},
      getReleaseByTag: async () => {
        if (status !== 200) throw {status: status === 'draft' ? 404 : status};
        return {data: {html_url: 'https://example.test/release', draft: false}};
      },
      createRelease: async params => calls.push(params)
    }}};
    let error;
    try {
      await publishRelease(github, {repo: {owner: 'scylladb', repo: 'alternator-client-cpp'}},
        {env: {RELEASE_TAG: 'v1.2.3', EXPECTED_TAG_OBJECT: 'verified-tag-object'}}, {info: () => {}});
    } catch (caught) { error = caught; }
    assert.equal(Boolean(error), status !== 200 && status !== 404);
    assert.equal(calls.length, status === 404 ? 1 : 0);
    if (calls.length) assert.deepEqual(calls[0], {
      owner: 'scylladb', repo: 'alternator-client-cpp', tag_name: 'v1.2.3',
      name: 'v1.2.3', generate_release_notes: true
    });
  }
});

test('draft discovery errors fail without publication; unrelated drafts do not block the tag', async () => {
  for (const failListing of [false, true]) {
    const calls = [];
    const github = {paginate: async () => {
      if (failListing) throw new Error('Release listing unavailable');
      return [{tag_name: 'another-tag', draft: true}, {tag_name: 'v1.2.3', draft: false}];
    }, rest: {repos: {
      listReleases: () => {},
      getReleaseByTag: async () => {
        calls.push('lookup');
        return {data: {html_url: 'https://example.test/release', draft: false}};
      },
      createRelease: async () => calls.push('create')
    }}};
    const run = () => publishRelease(github, {repo: {owner: 'scylladb', repo: 'alternator-client-cpp'}},
      {env: {RELEASE_TAG: 'v1.2.3'}}, {info: () => {}});
    if (failListing) await assert.rejects(run(), /listing unavailable/);
    else await run();
    assert.deepEqual(calls, failListing ? [] : ['lookup']);
  }
});

test('creation rechecks the validated tag after release lookup and rejects missing or changed objects', async () => {
  for (const state of ['missing', 'changed', 'unavailable', 'verified-tag-object']) {
    const calls = [];
    const github = {paginate: async () => {
      calls.push('list');
      return [];
    }, rest: {git: {getRef: async params => {
      calls.push('ref');
      assert.equal(params.ref, 'tags/v1.2.3');
      if (state === 'missing' || state === 'unavailable') throw {status: state === 'missing' ? 404 : 503};
      return {data: {object: {sha: state}}};
    }}, repos: {
      listReleases: () => {},
      getReleaseByTag: async () => { calls.push('lookup'); throw {status: 404}; },
      createRelease: async () => calls.push('create')
    }}};
    const run = () => publishRelease(github, {repo: {owner: 'scylladb', repo: 'alternator-client-cpp'}},
      {env: {RELEASE_TAG: 'v1.2.3', EXPECTED_TAG_OBJECT: 'verified-tag-object'}}, {info: () => {}});
    if (state === 'verified-tag-object') await run();
    else await assert.rejects(run);
    assert.deepEqual(calls, ['list', 'lookup', 'ref', ...(state === 'verified-tag-object' ? ['create'] : [])]);
  }
});

test('publication rejects a changed remote tag object', () => {
  const verify = releaseWorkflow.split(/^      - name: Recheck published tag before GitHub release\n/m)[1]
    .split(/^        run: \|\n/m)[1].split(/\n(?= {0,9}\S)/, 1)[0]
    .split('\n').map(line => line.replace(/^          /, '')).join('\n');
  const dir = mkdtempSync(join(tmpdir(), 'release-tag-'));
  try {
    writeFileSync(join(dir, 'git'), '#!/bin/sh\nprintf "remote-object\\trefs/tags/v1.2.3\\n"\n');
    chmodSync(join(dir, 'git'), 0o755);
    for (const [object, status] of [['remote-object', 0], ['wrong-object', 1]]) {
      const result = spawnSync('bash', ['-c', verify], {
        encoding: 'utf8', timeout: 30_000, killSignal: 'SIGKILL', env: {
        ...process.env, PATH: `${dir}:${process.env.PATH}`,
        RELEASE_TAG: 'v1.2.3', EXPECTED_TAG_OBJECT: object
      }});
      assert.ifError(result.error);
      assert.equal(result.status, status, result.stderr);
    }
  } finally { rmSync(dir, {recursive: true}); }
});
