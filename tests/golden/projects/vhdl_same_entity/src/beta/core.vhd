-- Library beta, also file core.vhd: rotate left by one (entity core).
library ieee;
use ieee.std_logic_1164.all;

entity core is
    port (a : in std_logic_vector(3 downto 0);
          y : out std_logic_vector(3 downto 0));
end entity core;

architecture rtl of core is
begin
    y <= a(2 downto 0) & a(3);
end architecture rtl;
