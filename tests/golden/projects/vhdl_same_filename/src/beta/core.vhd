-- Library beta, also file core.vhd: rotate left by one.
library ieee;
use ieee.std_logic_1164.all;

entity rot4 is
    port (a : in std_logic_vector(3 downto 0);
          y : out std_logic_vector(3 downto 0));
end entity rot4;

architecture rtl of rot4 is
begin
    y <= a(2 downto 0) & a(3);
end architecture rtl;
