-- VHDL top instantiating the Verilog module vadd through a component declaration.
library ieee;
use ieee.std_logic_1164.all;

entity top is
    port (clk  : in std_logic;
          a, b : in std_logic_vector(3 downto 0);
          q    : out std_logic_vector(3 downto 0));
end entity top;

architecture rtl of top is
    component vadd is
        port (a, b : in std_logic_vector(3 downto 0);
              s    : out std_logic_vector(3 downto 0));
    end component vadd;
    signal s : std_logic_vector(3 downto 0);
begin
    u_add : vadd port map (a => a, b => b, s => s);
    process (clk)
    begin
        if rising_edge(clk) then
            q <= s;
        end if;
    end process;
end architecture rtl;
