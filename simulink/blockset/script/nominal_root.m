function root = nominal_root()
%NOMINAL_ROOT  The folder the Nominal blockset is installed in.
%
%   The blockset's files find each other through this rather than through the MATLAB
%   path, because where they live depends on how they got there: an installed toolbox
%   sits under the add-ons folder, a clone wherever it was cloned.

    root = fileparts(fileparts(mfilename('fullpath')));
end
