-- y = 3 * x (mod 16), instantiated from Verilog.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity scale is
    port (x : in std_logic_vector(3 downto 0);
          y : out std_logic_vector(3 downto 0));
end entity scale;

architecture rtl of scale is
begin
    y <= std_logic_vector(resize(unsigned(x) * 3, 4));
end architecture rtl;
