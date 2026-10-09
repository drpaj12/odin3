-- The body of util_pkg, in another file than the package: analysed after pkg.vhd.
package body util_pkg is
    function swap (v : std_logic_vector(3 downto 0)) return std_logic_vector is
    begin
        return v(1 downto 0) & v(3 downto 2);
    end function swap;
end package body util_pkg;
