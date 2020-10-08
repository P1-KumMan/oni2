/*
 * BufferLine.re
 *
 * In-memory text buffer representation
 */
exception OutOfBounds;

open EditorCoreTypes;

module Log = (val Timber.Log.withNamespace("Oni2.Core.BufferLine"));

type measure = Uchar.t => float;

//let _space = Uchar.of_char(' ');
//let tab = Uchar.of_char('\t');
//let _cr = Uchar.of_char('\r');
//let _lf = Uchar.of_char('\n');

type characterCacheInfo = {
  byteOffset: int,
  positionPixelOffset: float,
  uchar: Uchar.t,
  pixelWidth: float,
};

// We use these 'empty' values to reduce allocations in the normal workflow
// of creating buffer lines. We defer actually creating the arrays / caches
// until we need them.

// This is important for performance for loading large files, and prevents
// us from needing to allocate arrays for every line upon load of the buffer.

// These 'placeholder' values are treated like a [None] - we can use a reference
// equality check against these to see if we've allocated already.
let emptyCharacterMap: array(option(characterCacheInfo)) = [||];
let emptyByteIndexMap: array(option(int)) = [||];

type t = {
  // [raw] is the raw string (byte array)
  raw: string,
  // [skiaTypeface] is the Skia typeface associated with the font
  measure: Uchar.t => float,
  // [characters] is a cache of discovered characters we've found in the string so far
  mutable characters: array(option(characterCacheInfo)),
  // [byteIndexMap] is a cache of byte -> index
  mutable byteIndexMap: array(option(int)),
  // [nextByte] is the nextByte to work from, or -1 if complete
  mutable nextByte: int,
  // [nextIndex] is the nextIndex to work from
  mutable nextIndex: int,
  // [nextGlyphStringIndex] is the next nth glyph string in shapes
  mutable nextGlyphStringByte: int,
  // [nextPixelPosition] is the graphical position (based on pixels)
  mutable nextPixelPosition: float,
};

module Internal = {
  let skiaTypefaceEqual = (tf1, tf2) =>
    Skia.Typeface.getUniqueID(tf1) == Skia.Typeface.getUniqueID(tf2);

  // We want to cache measurements tied to typefaces and characters, since text measuring is expensive
  module SkiaTypefaceUcharHashable = {
    // This is of the form typeface, size, uchar
    type t = (Skia.Typeface.t, float, Revery.Font.Smoothing.t, Uchar.t);

    let equal = ((tf1, sz1, sm1, uc1), (tf2, sz2, sm2, uc2)) =>
      skiaTypefaceEqual(tf1, tf2)
      && Float.equal(sz1, sz2)
      && sm1 == sm2
      && Uchar.equal(uc1, uc2);

    let hash = ((tf, sz, sm, uc)) =>
      Int32.to_int(Skia.Typeface.getUniqueID(tf))
      + Uchar.hash(uc)
      + Hashtbl.hash(sz)
      + Hashtbl.hash(sm);
  };

  module MeasureResult = {
    type t = float;

    let weight = _ => 1;
  };

  module MeasurementsCache =
    Lru.M.Make(SkiaTypefaceUcharHashable, MeasureResult);

  //  let measurementsCache =
  //    MeasurementsCache.create(~initialSize=1024, 128 * 1024);

  // Create a paint to measure the character with
  let paint = Skia.Paint.make();
  Skia.Paint.setTextEncoding(paint, GlyphId);
  Skia.Paint.setLcdRenderText(paint, true);

  //  let measure = (~typeface: Skia.Typeface.t, ~cache: t, uchar) =>
  //    switch (
  //      MeasurementsCache.find(
  //        (typeface, cache.font.fontSize, cache.font.smoothing, uchar),
  //        measurementsCache,
  //      )
  //    ) {
  //    | Some(pixelWidth) =>
  //      MeasurementsCache.promote(
  //        (typeface, cache.font.fontSize, cache.font.smoothing, uchar),
  //        measurementsCache,
  //      );
  //      Log.tracef(m =>
  //        m(
  //          "MeasurementCache : Hit! Typeface : %s, Font Size: %f, Uchar: %s (%d)",
  //          Skia.Typeface.getFamilyName(typeface),
  //          cache.font.fontSize,
  //          Zed_utf8.singleton(uchar),
  //          Uchar.to_int(uchar),
  //        )
  //      );
  //      pixelWidth;
  //    | None =>
  //      Log.tracef(m =>
  //        m(
  //          "MeasurementCache : Miss! Typeface : %s, Uchar: %s (%d)",
  //          Skia.Typeface.getFamilyName(typeface),
  //          Zed_utf8.singleton(uchar),
  //          Uchar.to_int(uchar),
  //        )
  //      );
  //      Skia.Paint.setTypeface(paint, typeface);
  //      // When the character is a tab, we have to make sure
  //      // we offset the correct amount.
  //      let pixelWidth =
  //        if (Uchar.equal(uchar, tab)) {
  //          float(cache.indentation.tabSize) *. cache.font.spaceWidth;
  //        } else {
  //          cache.measure(uchar);
  //        };
  //      MeasurementsCache.add(
  //        (typeface, cache.font.fontSize, cache.font.smoothing, uchar),
  //        pixelWidth,
  //        measurementsCache,
  //      );
  //      MeasurementsCache.trim(measurementsCache);
  //      pixelWidth;
  //    };

  let resolveTo = (~index: CharacterIndex.t, cache: t) => {
    // First, allocate our cache, if necessary
    if (cache.characters === emptyCharacterMap) {
      cache.characters = Array.make(String.length(cache.raw), None);
    };

    if (cache.byteIndexMap === emptyByteIndexMap) {
      cache.byteIndexMap = Array.make(String.length(cache.raw), None);
    };

    let characterIndexInt = CharacterIndex.toInt(index);

    // We've already resolved to this point,
    // no work needed!
    if (characterIndexInt < cache.nextIndex) {
      ();
    } else {
      // Requested an index we haven't discovered yet - so we'll need to compute up to the point
      let len = String.length(cache.raw);

      let i: ref(int) = ref(cache.nextIndex);
      let byte: ref(int) = ref(cache.nextByte);
      let pixelPosition: ref(float) = ref(cache.nextPixelPosition);

      //      let glyphStrings: ref(list((Skia.Typeface.t, string))) =
      //        ref(cache.glyphStrings);
      //      let glyphStringByte: ref(int) = ref(cache.nextGlyphStringByte);

      //      Skia.Paint.setTextSize(paint, cache.font.fontSize);
      //      Revery.Font.Smoothing.setPaint(~smoothing=cache.font.smoothing, paint);

      while (i^ <= characterIndexInt && byte^ < len) {
        let (uchar, offset) =
          ZedBundled.unsafe_extract_next(cache.raw, byte^);

        let pixelWidth = cache.measure(uchar);
        //measure(~typeface=cache.skiaTypeface, ~cache, uchar);

        Log.tracef(m =>
          m(
            "resolveTo loop: uchar : %s, pixelPosition : %f",
            Zed_utf8.singleton(uchar),
            pixelPosition^,
          )
        );

        let idx = i^;
        let byteOffset = byte^;
        cache.characters[idx] =
          Some({
            byteOffset,
            positionPixelOffset: pixelPosition^,
            uchar,
            pixelWidth,
          });

        cache.byteIndexMap[byteOffset] = Some(idx);

        pixelPosition := pixelPosition^ +. pixelWidth;
        byte := offset;
        incr(i);
      };

      cache.nextIndex = i^;
      cache.nextByte = byte^;
      cache.nextPixelPosition = pixelPosition^;
    };
  };
};

let make = (~measure, raw: string) => {
  {
    // Create a cache the size of the string - this would be the max length
    // of the UTF8 string, if it was all 1-byte unicode characters (ie, an ASCII string).

    measure,
    raw,
    characters: emptyCharacterMap,
    byteIndexMap: emptyByteIndexMap,
    nextByte: 0,
    nextIndex: 0,
    nextGlyphStringByte: 0,
    nextPixelPosition: 0.,
  };
};

let empty = (~measure, ()) => make(~measure, "");

let lengthInBytes = ({raw, _}) => String.length(raw);

let lengthSlow = ({raw, _}) => ZedBundled.length(raw);

let raw = ({raw, _}) => raw;

let lengthBounded = (~max, bufferLine) => {
  Internal.resolveTo(~index=max, bufferLine);
  min(bufferLine.nextIndex, CharacterIndex.toInt(max));
};

let getIndex = (~byte, bufferLine) => {
  let byteIdx = ByteIndex.toInt(byte);

  // The maximum character index this byte could be is literally [byte]
  // (ie, in the case where the string is all ASCII / 1-byte characters)
  // So resolve to that point
  let maximumPossibleCharacterIndex =
    CharacterIndex.ofInt(ByteIndex.toInt(byte));
  Internal.resolveTo(~index=maximumPossibleCharacterIndex, bufferLine);

  // In the case where we are looking at an 'intermediate' byte,
  // work backwards to the previous matching index.
  let rec loop = byteIdx =>
    if (byteIdx <= 0) {
      0;
    } else {
      switch (bufferLine.byteIndexMap[byteIdx]) {
      | Some(v) => v
      | None => loop(byteIdx - 1)
      };
    };

  // If we're asking for a byte past the length of the string,
  // return the index that would be past the last index. The reason
  // we handle this case - as opposed throw - is to handle the
  // case where a cursor position is past the end of the current string.
  if (byteIdx >= String.length(bufferLine.raw)) {
    bufferLine.nextIndex |> CharacterIndex.ofInt;
  } else {
    loop(ByteIndex.toInt(byte)) |> CharacterIndex.ofInt;
  };
};

let getUcharExn = (~index, bufferLine) => {
  Internal.resolveTo(~index, bufferLine);
  let characters = bufferLine.characters;
  switch (characters[CharacterIndex.toInt(index)]) {
  | Some({uchar, _}) => uchar
  | None => raise(OutOfBounds)
  };
};

let getByteFromIndex = (~index, bufferLine) => {
  Internal.resolveTo(~index, bufferLine);
  let rawLength = String.length(bufferLine.raw);
  let characters = bufferLine.characters;
  let len = Array.length(characters);
  let characterIdx = CharacterIndex.toInt(index);
  let byteIdx =
    if (characterIdx < 0) {
      0;
    } else if (characterIdx >= len) {
      rawLength;
    } else {
      switch (characters[characterIdx]) {
      | Some({byteOffset, _}) => byteOffset
      | None => rawLength
      };
    };
  byteIdx |> ByteIndex.ofInt;
};

let subExn = (~index: CharacterIndex.t, ~length: int, bufferLine) => {
  let startOffset = getByteFromIndex(~index, bufferLine) |> ByteIndex.toInt;
  let endOffset =
    getByteFromIndex(~index=CharacterIndex.(index + length), bufferLine)
    |> ByteIndex.toInt;
  String.sub(bufferLine.raw, startOffset, endOffset - startOffset);
};

let getPixelPositionAndWidth = (~index: CharacterIndex.t, bufferLine: t) => {
  Internal.resolveTo(~index, bufferLine);
  let characters = bufferLine.characters;
  let len = Array.length(characters);

  let characterIdx = CharacterIndex.toInt(index);

  let spaceWidth = bufferLine.measure(Uchar.of_char(' '));

  if (characterIdx < 0 || characterIdx >= len || len == 0) {
    (bufferLine.nextPixelPosition, spaceWidth);
  } else {
    switch (characters[characterIdx]) {
    | Some({positionPixelOffset, pixelWidth, _}) => (
        positionPixelOffset,
        pixelWidth,
      )
    | None =>
      switch (characters[bufferLine.nextIndex - 1]) {
      | Some({positionPixelOffset, pixelWidth, _}) => (
          positionPixelOffset +. pixelWidth,
          spaceWidth,
        )
      | None => (0., spaceWidth)
      }
    };
  };
};

module Slow = {
  let getIndexFromPixel = (~pixel, bufferLine) => {
    let characterLength = lengthSlow(bufferLine);
    let rec loop = (low, high) =>
      if (high == low) {
        high;
      } else if (high < low) {
        characterLength - 1;
      } else {
        let mid = (low + high) / 2;
        let (midPixel, midPixelWidth) =
          getPixelPositionAndWidth(
            ~index=CharacterIndex.ofInt(mid),
            bufferLine,
          );
        if (pixel < midPixel) {
          loop(low, mid - 1);
        } else if (pixel > midPixel +. midPixelWidth) {
          loop(mid + 1, high);
        } else {
          mid;
        };
      };

    loop(0, characterLength - 1) |> CharacterIndex.ofInt;
  };
};
