function blkStruct = slblocks
%SLBLOCKS  Register the Nominal blockset with the Simulink Library Browser.
%
%   Simulink calls every slblocks on the path when it builds the Library Browser. Keep
%   this file beside nominal_lib.slx so both are on the path together. The library must
%   also have been saved with EnableLBRepository on, which make_library does.
%
%   Blockset Designer reads this file too: it is what an import of the blockset is
%   pointed at.

    Browser.Library = 'nominal_lib';
    Browser.Name    = 'Nominal';

    blkStruct.Browser = Browser;
end
