function table = forwarding_table()
%FORWARDING_TABLE  How nominal_lib's past blocks map onto its current ones.
%
%   Users' models store a link to 'nominal_lib/<block name>'. Rename a block, move it to
%   a sublibrary, or rename or remove a parameter, and every saved model holding the old
%   one breaks, unless the library carries a forwarding table entry, which Simulink
%   applies as it opens the model. make_library writes this table into the library.
%
%   Empty until the first such change ships. Each entry is one of:
%
%     {'nominal_lib/Old Name', 'nominal_lib/New Name'}
%         a rename or a move; versions do not apply
%
%     {'nominal_lib/Block', 'nominal_lib/Block', '0.1', '0.2', 'transformFcn'}
%         a parameter change between two library versions. transformFcn is a function on
%         the blockset path taking inData and returning outData, which rewrites the old
%         instance's parameters. Versions are the library's ModelVersion, which
%         make_library pins to major.minor from simulink/VERSION.
%
%   To rename a mask parameter, prefer giving the new parameter the old name as an Alias
%   over a transform: set_param then accepts either.
%
%   See "Maintain Compatibility of Library Blocks Using Forwarding Tables" in the
%   Simulink documentation.

    table = {};
end
