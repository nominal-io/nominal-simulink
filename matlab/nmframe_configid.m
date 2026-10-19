function id = nmframe_configid(datasetRid, channels, tags)
%NMFRAME_CONFIGID  The config_id for a dataset, ordered channel list and tags.
%
%   id = NMFRAME_CONFIGID(rid, channels, tags) returns a uint64 matching what the C and
%   Go implementations compute for the same input. `channels` is a cellstr, in order.
%   `tags` is an N-by-2 cell array of {key, value} rows, in order, and can be omitted
%   when there are none.
%
%   FNV-1a over each field length-prefixed as a 4-byte little-endian count followed by
%   its bytes: first the RID, then each channel name, then each tag's key and value. The
%   length prefixes are what stop {'ab','c'} and {'a','bc'} hashing the same.
%
%       h = 0xcbf29ce484222325
%       for each byte b:  h = (h XOR b) * 0x100000001b3   (mod 2^64)
%
%   A sender must compute this exactly. nmproxy recomputes it from each HELLO and
%   rejects one that does not match, with a warning, after which no data arrives. Check
%   any new implementation against the canonical vector first — FNV-1a/64 of the single
%   byte 'a' is 0xaf63dc4c8601ec8c.
%
%   A relay does not strictly need this. It can treat config_id as an opaque key and
%   simply match DATA frames to the HELLO that declared the same value. Recomputing it at
%   HELLO catches a sender whose hash disagrees with nmproxy's.
%
%   See also NMFRAME_DECODE

    % Hexadecimal literals, not hex2dec. hex2dec returns a *double*, so a value above
    % 2^53 is rounded before uint64() ever sees it. MATLAB warns, but the result is a
    % constant close to the right one that produces a hash matching nothing. Literals
    % are exact.
    OFFSET = 0xcbf29ce484222325u64;
    PRIME  = 0x100000001b3u64;

    if nargin < 2
        channels = {};
    end
    if nargin < 3
        tags = cell(0, 2);
    end
    if ischar(channels)
        channels = {channels};
    end

    h = hashField(OFFSET, PRIME, datasetRid);
    for i = 1:numel(channels)
        h = hashField(h, PRIME, channels{i});
    end
    for i = 1:size(tags, 1)
        h = hashField(h, PRIME, tags{i, 1});
        h = hashField(h, PRIME, tags{i, 2});
    end
    id = h;
end

function h = hashField(h, prime, s)
% Length prefix, then the bytes.
    b = uint8(s(:).');
    n = uint32(numel(b));
    lenLE = typecast(n, 'uint8');   % little-endian on every platform MATLAB runs on
    h = hashBytes(h, prime, lenLE);
    h = hashBytes(h, prime, b);
end

function h = hashBytes(h, prime, bytes)
    for k = 1:numel(bytes)
        h = bitxor(h, uint64(bytes(k)));
        % Not h * prime: MATLAB's uint64 multiply saturates instead of wrapping, which
        % would silently produce a hash that agrees with nothing. See nmframe_mulmod64.
        h = nmframe_mulmod64(h, prime);
    end
end
