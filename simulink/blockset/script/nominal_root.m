function root = nominal_root()
%NOMINAL_ROOT  The blockset's root folder.
%
%   That is the install folder of an installed toolbox, or simulink/blockset in a clone.
%   nominal_help and the documentation find the blockset's files through this rather
%   than through the MATLAB path, because where they live depends on how they got there:
%   an installed toolbox sits under the add-ons folder, a clone wherever it was cloned.

    root = fileparts(fileparts(mfilename('fullpath')));
end
