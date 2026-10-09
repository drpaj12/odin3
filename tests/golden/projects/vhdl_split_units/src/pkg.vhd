-- Package util_pkg; its body is in pkg_body.vhd.
library ieee;
use ieee.std_logic_1164.all;

package util_pkg is
    function swap (v : std_logic_vector(3 downto 0)) return std_logic_vector;
end package util_pkg;
