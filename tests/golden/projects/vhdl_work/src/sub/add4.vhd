-- 4-bit adder (wraps around).
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity add4 is
    port (a, b : in std_logic_vector(3 downto 0);
          s    : out std_logic_vector(3 downto 0));
end entity add4;

architecture rtl of add4 is
begin
    s <= std_logic_vector(unsigned(a) + unsigned(b));
end architecture rtl;
