-- Entity core; its architecture is in core_arch.vhd.
library ieee;
use ieee.std_logic_1164.all;

entity core is
    port (x : in std_logic_vector(3 downto 0);
          y : out std_logic_vector(3 downto 0));
end entity core;
