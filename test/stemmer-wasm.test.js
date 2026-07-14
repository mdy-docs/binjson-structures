/**
 * Conformance test for the WASM Porter stemmer (c/stemmer.c) against the npm
 * `stemmer` package it ports. Comparing to the live package (rather than
 * hard-coded expectations) also guards against drift if `stemmer` is upgraded:
 * if the two ever diverge, the C port needs re-checking.
 *
 * The full-dictionary sweep (~236k words) lives in a dev harness; here we cover
 * a representative corpus exercising every Porter step, run in-process.
 */
import { describe, it, expect, beforeAll } from 'vitest';
import { stemmer as jsStemmer } from 'stemmer';
import { ready, stemmer as wasmStemmer } from '../wasm/binjson-structures-wasm.js';

// Words chosen to exercise steps 1a–5, the y-handling, *v*/*o conditions and
// the suffix tables (many are the canonical examples from Porter's paper).
const WORDS = [
  '', 'a', 'ab', 'the', 'is', 'sky', 'skies', 'happy', 'happier', 'by', 'y',
  'yes', 'yellow', 'dying', 'lying', 'yy',
  // 1a
  'caresses', 'ponies', 'ties', 'caress', 'cats', 'boss', 'dogs', 'gas',
  // 1b
  'feed', 'agreed', 'plastered', 'bled', 'motoring', 'sing', 'conflated',
  'troubled', 'sized', 'hopping', 'tanned', 'falling', 'hissing', 'fizzed',
  'failing', 'filing',
  // 1c
  'happy', 'sky', 'cry', 'say',
  // step 2
  'relational', 'conditional', 'rational', 'valenci', 'hesitanci', 'digitizer',
  'conformabli', 'radicalli', 'differentli', 'vileli', 'analogousli',
  'vietnamization', 'predication', 'operator', 'feudalism', 'decisiveness',
  'hopefulness', 'callousness', 'formaliti', 'sensitiviti', 'sensibiliti',
  // step 3
  'triplicate', 'formative', 'formalize', 'electriciti', 'electrical',
  'hopeful', 'goodness',
  // step 4
  'revival', 'allowance', 'inference', 'airliner', 'gyroscopic', 'adjustable',
  'defensible', 'irritant', 'replacement', 'adjustment', 'dependent', 'adoption',
  'homologou', 'communism', 'activate', 'angulariti', 'homologous', 'effective',
  'bowdlerize',
  // step 5
  'probate', 'rate', 'cease', 'controll', 'roll',
  // casing / non-letters / short
  'ABCDEF', 'HeLLo', 'Y', 'YY', 'ex', 'exed', 'axes', 'running', 'runner',
  'connection', 'connections', 'generalization', 'stemming', 'consign',
  'consigned', 'consigning', 'knack', 'knackeries'
];

describe('WASM Porter stemmer matches npm stemmer', () => {
  beforeAll(async () => {
    await ready();
  });

  it('produces byte-identical stems across the corpus', () => {
    for (const word of WORDS) {
      expect(wasmStemmer(word), `stem of ${JSON.stringify(word)}`).toBe(jsStemmer(word));
    }
  });

  it('handles the canonical Porter examples', () => {
    expect(wasmStemmer('caresses')).toBe('caress');
    expect(wasmStemmer('ponies')).toBe('poni');
    expect(wasmStemmer('relational')).toBe('relat');
    expect(wasmStemmer('happy')).toBe('happi');
    expect(wasmStemmer('sky')).toBe('sky');
  });
});
