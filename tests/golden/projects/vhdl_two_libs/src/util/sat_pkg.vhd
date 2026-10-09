-- Library util: the saturation limit.
library ieee;
use ieee.std_logic_1164.all;

package sat_pkg is
    constant LIMIT : std_logic_vector(3 downto 0) := "1100";
end package sat_pkg;
