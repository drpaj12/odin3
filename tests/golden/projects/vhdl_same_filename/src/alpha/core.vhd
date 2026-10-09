-- Library alpha, file core.vhd: inverter.
library ieee;
use ieee.std_logic_1164.all;

entity inv4 is
    port (a : in std_logic_vector(3 downto 0);
          y : out std_logic_vector(3 downto 0));
end entity inv4;

architecture rtl of inv4 is
begin
    y <= not a;
end architecture rtl;
