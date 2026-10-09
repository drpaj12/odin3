-- y = x + STEP; analysed after consts_pkg.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.consts_pkg.all;

entity inc is
    port (x : in std_logic_vector(3 downto 0);
          y : out std_logic_vector(3 downto 0));
end entity inc;

architecture rtl of inc is
begin
    y <= std_logic_vector(unsigned(x) + STEP);
end architecture rtl;
