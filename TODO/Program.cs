using System.Threading.Tasks;

namespace NanoBridgeApp
{
    internal class Program
    {
        static async Task Main(string[] args)
        {
            var app = new BridgeSimple();
            await app.RunAsync();
        }
    }
}