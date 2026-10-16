function blkStruct = slblocks
%SLBLOCKS  Register the Nominal blockset with the Simulink Library Browser.
%
%   Simulink calls every slblocks on the path when it builds the Library Browser, so this
%   must sit beside nominal_lib.slx. The library must also have been saved with
%   EnableLBRepository on, which make_library does.
%
%   Blockset Designer reads this file too: it is what an import of the blockset is
%   pointed at.

    Browser.Library = 'nominal_lib';
    Browser.Name    = 'Nominal';

    blkStruct.Browser = Browser;
end
