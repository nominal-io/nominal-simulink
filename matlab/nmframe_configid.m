function id = nmframe_configid(datasetRid, channels)
%NMFRAME_CONFIGID  The config_id for a dataset and ordered channel list.
%
%   id = NMFRAME_CONFIGID(rid, channels) returns a uint64 matching what the C and Go
%   implementations compute for the same input. `channels` is a cellstr, in order.
%
%   FNV-1a over each field length-prefixed as a 4-byte little-endian count followed by
%   its bytes: first the RID, then each channel name. The length prefixes are what stop
%   {'ab','c'} and {'a','bc'} hashing the same.
%
%       h = 0xcbf29ce484222325
%       for each byte b:  h = (h XOR b) * 0x100000001b3   (mod 2^64)
%
%   Getting this wrong has no visible symptom: every DATA frame is rejected as an
%   unknown configuration, no data arrives, and nothing reports an error. Check any new
%   implementation against the canonical vector first — FNV-1a/64 of the single byte 'a'
%   is 0xaf63dc4c8601ec8c.
%
%   A proxy does not strictly need this. It can treat config_id as an opaque key and
%   simply match DATA frames to the HELLO that declared the same value. Computing it is
%   how you catch a sender whose hash disagrees with yours, which is worth doing once at
%   HELLO rather than discovering later that nothing ever matched.
%
%   See also NMFRAME_DECODE

    % Hexadecimal literals, not hex2dec. hex2dec returns a *double*, so a value above
    % 2^53 is silently rounded before uint64() ever sees it — MATLAB warns, and the
    % result is a constant that is close to the right one and produces a hash matching
    % nothing. Literals are exact.
    OFFSET = 0xcbf29ce484222325u64;
    PRIME  = 0x100000001b3u64;

    if nargin < 2
        channels = {};
    end
    if ischar(channels)
        channels = {channels};
    end

    h = hashField(OFFSET, PRIME, datasetRid);
    for i = 1:numel(channels)
        h = hashField(h, PRIME, channels{i});
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
        % would silently produce a hash that agrees with nothing. See the function.
        h = nmframe_mulmod64(h, prime);
    end
end
