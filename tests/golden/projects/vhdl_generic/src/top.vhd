-- W-bit counter; the project overrides the generic W (default 4) with 6.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity top is
    generic (W : positive := 4);
    port (clk : in std_logic;
          q   : out std_logic_vector(W - 1 downto 0));
end entity top;

architecture rtl of top is
    signal count : unsigned(W - 1 downto 0) := (others => '0');
begin
    process (clk)
    begin
        if rising_edge(clk) then
            count <= count + 1;
        end if;
    end process;
    q <= std_logic_vector(count);
end architecture rtl;
