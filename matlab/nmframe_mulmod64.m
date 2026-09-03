function r = nmframe_mulmod64(a, b)
%NMFRAME_MULMOD64  (a * b) mod 2^64 for uint64 inputs.
%
%   Exists because MATLAB's uint64 arithmetic *saturates* rather than wrapping:
%
%       >> intmax('uint64') + 1
%       ans = 18446744073709551615
%
%   A hash written the obvious way — h = h * PRIME — therefore pegs at the maximum on
%   the first multiply and stays there, producing a stable, plausible, and completely
%   wrong value that agrees with no other implementation. This is the single most likely
%   way to get FNV-1a wrong in MATLAB, and it fails silently.
%
%   So the multiply is done on 16-bit limbs in double precision. Every partial product
%   is at most (2^16-1)^2, just under 2^32, and every accumulator stays under about
%   2^34 — both exact in a double, which is integer-exact to 2^53. Limbs that would land
%   at or above 2^64 are simply not computed, which is the modulus.
%
%   See also NMFRAME_CONFIGID

    a16 = limbs16(a);
    b16 = limbs16(b);

    acc = zeros(1, 4);
    for i = 1:4
        for j = 1:(5 - i)          % i+j-2 <= 3; anything higher falls off the top
            acc(i + j - 1) = acc(i + j - 1) + a16(i) * b16(j);
        end
    end

    out   = zeros(1, 4);
    carry = 0;
    for k = 1:4
        t      = acc(k) + carry;
        out(k) = mod(t, 65536);
        carry  = floor(t / 65536);
    end

    r = uint64(0);
    for k = 1:4
        % bitshift rather than multiplication, because shifting does not saturate.
        r = bitor(r, bitshift(uint64(out(k)), 16 * (k - 1)));
    end
end

function l = limbs16(v)
    l = zeros(1, 4);
    for k = 1:4
        l(k) = double(bitand(bitshift(v, -16 * (k - 1)), uint64(65535)));
    end
end
