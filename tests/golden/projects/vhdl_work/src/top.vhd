-- Accumulator: acc <= acc + x, built from two entities of library work.
library ieee;
use ieee.std_logic_1164.all;

entity top is
    port (clk, rst : in std_logic;
          x        : in std_logic_vector(3 downto 0);
          acc      : out std_logic_vector(3 downto 0));
end entity top;

architecture rtl of top is
    signal sum, cur : std_logic_vector(3 downto 0);
begin
    u_add : entity work.add4 port map (a => cur, b => x, s => sum);
    u_reg : entity work.reg4 port map (clk => clk, rst => rst, d => sum, q => cur);
    acc <= cur;
end architecture rtl;
